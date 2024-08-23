#include "kz_timer.h"
#include "kz/db/kz_db.h"
#include "../mode/kz_mode.h"
#include "../style/kz_style.h"
#include "../noclip/kz_noclip.h"
#include "../option/kz_option.h"
#include "../language/kz_language.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

static_global class KZDatabaseServiceEventListener_Timer : public KZDatabaseServiceEventListener
{
public:
	virtual void OnMapSetup() override;
} databaseEventListener;

static_global CUtlVector<KZTimerServiceEventListener *> eventListeners;

bool KZTimerService::RegisterEventListener(KZTimerServiceEventListener *eventListener)
{
	if (eventListeners.Find(eventListener) >= 0)
	{
		return false;
	}
	eventListeners.AddToTail(eventListener);
	return true;
}

bool KZTimerService::UnregisterEventListener(KZTimerServiceEventListener *eventListener)
{
	return eventListeners.FindAndRemove(eventListener);
}

void KZTimerService::StartZoneStartTouch(const KzCourseDescriptor *course)
{
	this->touchedGroundSinceTouchingStartZone = !!(this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND);
	this->TimerStop(false);
}

void KZTimerService::StartZoneEndTouch(const KzCourseDescriptor *course)
{
	if (this->touchedGroundSinceTouchingStartZone)
	{
		this->TimerStart(course);
	}
}

void KZTimerService::SplitZoneStartTouch(const KzCourseDescriptor *course, i32 splitNumber)
{
	if (!this->GetTimerRunning())
	{
		return;
	}

	assert(splitNumber > INVALID_SPLIT_NUMBER && splitNumber < KZ_MAX_SPLIT_ZONES);

	if (this->splitZoneTimes[splitNumber - 1] < 0)
	{
		this->PlayReachedSplitSound();
		this->splitZoneTimes[splitNumber - 1] = this->GetTime();
	}
}

void KZTimerService::CheckpointZoneStartTouch(const KzCourseDescriptor *course, i32 cpNumber)
{
	if (!this->GetTimerRunning())
	{
		return;
	}

	assert(cpNumber > INVALID_CHECKPOINT_NUMBER && cpNumber < KZ_MAX_CHECKPOINT_ZONES);

	if (this->cpZoneTimes[cpNumber - 1] < 0)
	{
		Msg("reached checkpoint %i\n", cpNumber);
		this->PlayReachedCheckpointSound();
		this->cpZoneTimes[cpNumber - 1] = this->GetTime();
		this->reachedCheckpoints++;
	}
	else
	{
		Msg("already reached checkpoint %i, time %f\n", cpNumber, this->cpZoneTimes[cpNumber - 1]);
	}
}

void KZTimerService::StageZoneStartTouch(const KzCourseDescriptor *course, i32 stageNumber)
{
	if (!this->GetTimerRunning())
	{
		return;
	}

	assert(stageNumber > INVALID_STAGE_NUMBER && stageNumber < KZ_MAX_STAGE_ZONES);

	if (stageNumber > this->currentStage + 1)
	{
		this->PlayMissedZoneSound();
		this->player->languageService->PrintChat(true, false, "Touched too high stage number (Missed stage)", this->currentStage + 1);
		return;
	}

	if (stageNumber == this->currentStage + 1)
	{
		this->stageZoneTimes[this->currentStage] = this->GetTime();
		this->PlayReachedStageSound();
		this->currentStage++;
	}
}

bool KZTimerService::TimerStart(const KzCourseDescriptor *course, bool playSound)
{
	// clang-format off
	if (!this->player->GetPlayerPawn()->IsAlive()
		|| this->JustStartedTimer()
		|| this->player->JustTeleported()
		|| this->player->inPerf
		|| this->player->noclipService->JustNoclipped()
		|| !this->HasValidMoveType()
		|| this->JustLanded()
		|| (this->GetTimerRunning() && !V_stricmp(course->name, this->currentCourse->name))
		|| (!(this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND) && !this->GetValidJump()))
	// clang-format on
	{
		return false;
	}
	if (V_strlen(this->player->modeService->GetModeName()) > KZ_MAX_MODE_NAME_LENGTH)
	{
		Warning("[KZ] Timer start failed: Mode name is too long!");
		return false;
	}

	bool allowStart = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowStart &= eventListeners[i]->OnTimerStart(this->player, course);
	}
	if (!allowStart)
	{
		return false;
	}

	this->currentTime = 0.0f;
	this->timerRunning = true;
	this->currentStage = 0;
	this->reachedCheckpoints = 0;

	f64 invalidTime = -1;
	this->splitZoneTimes.SetSize(course->splitCount);
	this->cpZoneTimes.SetSize(course->checkpointCount);
	this->stageZoneTimes.SetSize(course->checkpointCount);

	this->splitZoneTimes.FillWithValue(invalidTime);
	this->cpZoneTimes.FillWithValue(invalidTime);
	this->stageZoneTimes.FillWithValue(invalidTime);

	SetCourse(course);
	V_strncpy(this->lastStartMode, this->player->modeService->GetModeName(), KZ_MAX_MODE_NAME_LENGTH);
	validTime = true;
	if (playSound)
	{
		this->PlayTimerStartSound();
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerStartPost(this->player, course);
	}
	return true;
}

bool KZTimerService::TimerEnd(const KzCourseDescriptor *course)
{
	if (!this->player->IsAlive())
	{
		return false;
	}

	if (!this->timerRunning || V_stricmp(this->currentCourse->name, course->name) != 0)
	{
		this->PlayTimerFalseEndSound();
		this->lastFalseEndTime = g_pKZUtils->GetServerGlobals()->curtime;
		return false;
	}

	if (this->currentStage != course->stageCount)
	{
		this->PlayMissedZoneSound();
		this->player->languageService->PrintChat(true, false, "Can't finish run (Missed stage)", this->currentStage + 1);
		return false;
	}

	if (this->reachedCheckpoints != course->checkpointCount)
	{
		this->PlayMissedZoneSound();
		i32 missCount = course->checkpointCount - this->reachedCheckpoints;
		if (missCount == 1)
		{
			this->player->languageService->PrintChat(true, false, "Can't finish run (Missed a checkpoint zone)");
		}
		else
		{
			this->player->languageService->PrintChat(true, false, "Can't finish run (Missed checkpoint zones)", missCount);
		}
		return false;
	}

	f32 time = this->GetTime() + g_pKZUtils->GetServerGlobals()->frametime;
	u32 teleportsUsed = this->player->checkpointService->GetTeleportCount();

	bool allowEnd = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowEnd &= eventListeners[i]->OnTimerEnd(this->player, course, time, teleportsUsed);
	}
	if (!allowEnd)
	{
		return false;
	}
	// Update current time for one last time.
	this->currentTime = time;

	this->timerRunning = false;
	this->lastEndTime = g_pKZUtils->GetServerGlobals()->curtime;
	this->PlayTimerEndSound();

	if (!this->player->GetPlayerPawn()->IsBot())
	{
		KZ::timer::AddRunToAnnounceQueue(player, course, time, teleportsUsed);
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerEndPost(this->player, course, time, teleportsUsed);
	}

	return true;
}

bool KZTimerService::TimerStop(bool playSound)
{
	if (!this->timerRunning)
	{
		return false;
	}
	this->timerRunning = false;
	if (playSound)
	{
		this->PlayTimerStopSound();
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerStopped(this->player, this->currentCourse);
	}

	return true;
}

void KZTimerService::TimerStopAll(bool playSound)
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->timerService)
		{
			continue;
		}
		player->timerService->TimerStop(playSound);
	}
}

void KZTimerService::InvalidateJump()
{
	this->validJump = false;
	this->lastInvalidateTime = g_pKZUtils->GetServerGlobals()->curtime;
}

void KZTimerService::PlayTimerStartSound()
{
	if (g_pKZUtils->GetServerGlobals()->curtime - this->lastStartSoundTime > KZ_TIMER_SOUND_COOLDOWN)
	{
		utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_START);
		this->lastStartSoundTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
}

void KZTimerService::InvalidateRun()
{
	if (!this->validTime)
	{
		return;
	}
	this->validTime = false;

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerInvalidated(this->player);
	}
}

// =====[ PRIVATE ]=====

bool KZTimerService::HasValidMoveType()
{
	return KZTimerService::IsValidMoveType(this->player->GetMoveType());
}

bool KZTimerService::JustEndedTimer()
{
	return g_pKZUtils->GetServerGlobals()->curtime - this->lastEndTime > 1.0f;
}

void KZTimerService::PlayTimerEndSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_END);
}

void KZTimerService::PlayTimerFalseEndSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_FALSE_END);
}

void KZTimerService::PlayMissedZoneSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_MISSED_ZONE);
}

void KZTimerService::PlayReachedSplitSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_SPLIT);
}

void KZTimerService::PlayReachedCheckpointSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_CHECKPOINT);
}

void KZTimerService::PlayReachedStageSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_STAGE);
}

void KZTimerService::PlayTimerStopSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_STOP);
}

void KZTimerService::FormatTime(f64 time, char *output, u32 length, bool precise)
{
	int roundedTime = RoundFloatToInt(time * 1000); // Time rounded to number of ms

	int milliseconds = roundedTime % 1000;
	roundedTime = (roundedTime - milliseconds) / 1000;
	int seconds = roundedTime % 60;
	roundedTime = (roundedTime - seconds) / 60;
	int minutes = roundedTime % 60;
	roundedTime = (roundedTime - minutes) / 60;
	int hours = roundedTime;

	if (hours == 0)
	{
		if (precise)
		{
			snprintf(output, length, "%02i:%02i.%03i", minutes, seconds, milliseconds);
		}
		else
		{
			snprintf(output, length, "%i:%02i", minutes, seconds);
		}
	}
	else
	{
		if (precise)
		{
			snprintf(output, length, "%i:%02i:%02i.%03i", hours, minutes, seconds, milliseconds);
		}
		else
		{
			snprintf(output, length, "%i:%02i:%02i", hours, minutes, seconds);
		}
	}
}

static_function std::string GetTeleportCountText(int tpCount, const char *language)
{
	return tpCount == 1 ? KZLanguageService::PrepareMessageWithLang(language, "1 Teleport Text")
						: KZLanguageService::PrepareMessageWithLang(language, "2+ Teleports Text", tpCount);
}

void KZTimerService::Pause()
{
	if (!this->CanPause(true))
	{
		return;
	}

	bool allowPause = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowPause &= eventListeners[i]->OnPause(this->player);
	}
	if (!allowPause)
	{
		this->player->languageService->PrintChat(true, false, "Can't Pause (Generic)");
		this->player->PlayErrorSound();
		return;
	}

	this->paused = true;
	this->pausedOnLadder = this->player->GetMoveType() == MOVETYPE_LADDER;
	this->lastDuckValue = this->player->GetMoveServices()->m_flDuckAmount;
	this->lastStaminaValue = this->player->GetMoveServices()->m_flStamina;
	this->player->SetVelocity(vec3_origin);
	this->player->SetMoveType(MOVETYPE_NONE);

	if (this->GetTimerRunning())
	{
		this->hasPausedInThisRun = true;
		this->lastPauseTime = g_pKZUtils->GetServerGlobals()->curtime;
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnPausePost(this->player);
	}
}

bool KZTimerService::CanPause(bool showError)
{
	if (this->paused)
	{
		return false;
	}

	if (this->player->modifiers.disablePausingCount > 0)
	{
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Can't Pause (Anti Pause Area)");
		}
		return false;
	}

	Vector velocity;
	this->player->GetVelocity(&velocity);

	if (this->GetTimerRunning())
	{
		if (this->hasResumedInThisRun && g_pKZUtils->GetServerGlobals()->curtime - this->lastResumeTime < KZ_PAUSE_COOLDOWN)
		{
			if (showError)
			{
				this->player->languageService->PrintChat(true, false, "Can't Pause (Just Resumed)");
				this->player->PlayErrorSound();
			}
			return false;
		}
		else if (!this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND && !(velocity.Length2D() == 0.0f && velocity.z == 0.0f))
		{
			if (showError)
			{
				this->player->languageService->PrintChat(true, false, "Can't Pause (Just Resumed)");
				this->player->PlayErrorSound();
			}
			return false;
		}
		// TODO: Bhop/Antipause detection
	}
	return true;
}

void KZTimerService::Resume(bool force)
{
	if (!this->paused)
	{
		return;
	}
	if (!force && !this->CanResume(true))
	{
		return;
	}

	bool allowResume = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowResume &= eventListeners[i]->OnResume(this->player);
	}
	if (!allowResume)
	{
		this->player->languageService->PrintChat(true, false, "Can't Resume (Generic)");
		this->player->PlayErrorSound();
		return;
	}

	if (this->pausedOnLadder)
	{
		this->player->SetMoveType(MOVETYPE_LADDER);
	}
	else
	{
		this->player->SetMoveType(MOVETYPE_WALK);
	}

	// GOKZ: prevent noclip exploit
	this->player->GetPlayerPawn()->m_Collision().m_CollisionGroup() = KZ_COLLISION_GROUP_STANDARD;
	this->player->GetPlayerPawn()->CollisionRulesChanged();

	this->paused = false;
	if (this->GetTimerRunning())
	{
		this->hasResumedInThisRun = true;
		this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
	this->player->GetMoveServices()->m_flDuckAmount = this->lastDuckValue;
	this->player->GetMoveServices()->m_flStamina = this->lastStaminaValue;

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnResumePost(this->player);
	}
}

bool KZTimerService::CanResume(bool showError)
{
	if (this->GetTimerRunning() && this->hasPausedInThisRun && g_pKZUtils->GetServerGlobals()->curtime - this->lastPauseTime < KZ_PAUSE_COOLDOWN)
	{
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Can't Resume (Just Paused)");
			this->player->PlayErrorSound();
		}
		return false;
	}
	return true;
}

void KZTimerService::Reset()
{
	this->timerRunning = {};
	this->currentTime = {};
	this->currentCourse = nullptr;
	this->lastEndTime = {};
	this->lastFalseEndTime = {};
	this->lastStartSoundTime = {};
	this->lastStartMode[0] = 0;
	this->validTime = {};
	this->paused = {};
	this->pausedOnLadder = {};
	this->lastPauseTime = {};
	this->hasPausedInThisRun = {};
	this->lastResumeTime = {};
	this->hasResumedInThisRun = {};
	this->lastDuckValue = {};
	this->lastStaminaValue = {};
	this->validJump = {};
	this->lastInvalidateTime = {};
	this->touchedGroundSinceTouchingStartZone = {};
}

void KZTimerService::OnPhysicsSimulatePost()
{
	if (this->player->IsAlive() && this->GetTimerRunning() && !this->GetPaused())
	{
		this->currentTime += ENGINE_FIXED_TICK_INTERVAL;
	}
}

void KZTimerService::OnStartTouchGround()
{
	this->touchedGroundSinceTouchingStartZone = true;
}

void KZTimerService::OnStopTouchGround()
{
	if (this->HasValidMoveType() && this->lastInvalidateTime != g_pKZUtils->GetServerGlobals()->curtime)
	{
		this->validJump = true;
	}
	else
	{
		this->InvalidateJump();
	}
}

void KZTimerService::OnChangeMoveType(MoveType_t oldMoveType)
{
	if (oldMoveType == MOVETYPE_LADDER && this->player->GetMoveType() == MOVETYPE_WALK
		&& this->lastInvalidateTime != g_pKZUtils->GetServerGlobals()->curtime)
	{
		this->validJump = true;
	}
	else
	{
		this->InvalidateJump();
	}
	// Check if player has escaped MOVETYPE_NONE
	if (!this->paused || this->player->GetMoveType() == MOVETYPE_NONE)
	{
		return;
	}

	this->paused = false;
	if (this->GetTimerRunning())
	{
		this->hasResumedInThisRun = true;
		this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnResumePost(this->player);
	}
}

void KZTimerService::OnTeleportToStart()
{
	this->TimerStop();
}

void KZTimerService::OnClientDisconnect()
{
	this->TimerStop();
}

void KZTimerService::OnPlayerSpawn()
{
	if (!this->player->GetPlayerPawn() || !this->paused)
	{
		return;
	}

	// Player has left paused state by spawning in, so resume
	this->paused = false;
	if (this->GetTimerRunning())
	{
		this->hasResumedInThisRun = true;
		this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
	this->player->GetMoveServices()->m_flDuckAmount = this->lastDuckValue;
	this->player->GetMoveServices()->m_flStamina = this->lastStaminaValue;

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnResumePost(this->player);
	}
}

void KZTimerService::OnPlayerJoinTeam(i32 team)
{
	if (team == CS_TEAM_SPECTATOR)
	{
		this->paused = true;
		if (this->GetTimerRunning())
		{
			this->hasPausedInThisRun = true;
			this->lastPauseTime = g_pKZUtils->GetServerGlobals()->curtime;
		}

		FOR_EACH_VEC(eventListeners, i)
		{
			eventListeners[i]->OnPausePost(this->player);
		}
	}
}

void KZTimerService::OnPlayerDeath()
{
	this->TimerStop();
}

void KZTimerService::OnOptionsChanged()
{
	// TODO
}

void KZTimerService::OnRoundStart()
{
	KZTimerService::TimerStopAll();
}

void KZTimerService::OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity)
{
	if (newPosition || newVelocity)
	{
		this->InvalidateJump();
	}
}

static_function SCMD_CALLBACK(Command_KzStopTimer)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->timerService->GetTimerRunning())
	{
		player->timerService->TimerStop();
	}
	return MRES_SUPERCEDE;
}

static_function SCMD_CALLBACK(Command_KzPauseTimer)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->TogglePause();
	return MRES_SUPERCEDE;
}

void KZTimerService::Init()
{
	KZDatabaseService::RegisterEventListener(&databaseEventListener);
}

void KZTimerService::RegisterCommands()
{
	scmd::RegisterCmd("kz_stop", Command_KzStopTimer);
	scmd::RegisterCmd("kz_pause", Command_KzPauseTimer);
	KZTimerService::RegisterPBCommand();
	KZTimerService::RegisterRecordCommands();
}

void KZDatabaseServiceEventListener_Timer::OnMapSetup()
{
	KZ::timer::SetupCourses();
}
