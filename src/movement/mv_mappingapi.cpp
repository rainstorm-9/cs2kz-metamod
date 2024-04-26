
#include "kz/kz.h"
#include "movement.h"
#include "mv_mappingapi.h"
#include "entity2/entitykeyvalues.h"

#include "tier0/memdbgon.h"

#define KEY_TRIGGER_TYPE         "timer_trigger_type"
#define KEY_IS_COURSE_DESCRIPTOR "timer_course_descriptor"
#define INVALID_STAGE_NUMBER     -1

enum MvTriggerType
{
	MVTRIGGER_DISABLED = 0,
	MVTRIGGER_MODIFIER,
	MVTRIGGER_RESET_CHECKPOINTS,
	MVTRIGGER_SINGLE_BHOP_RESET,
	MVTRIGGER_ANTI_BHOP,

	MVTRIGGER_ZONE_START,
	MVTRIGGER_ZONE_END,
	MVTRIGGER_ZONE_SPLIT,
	MVTRIGGER_ZONE_CHECKPOINT,
	MVTRIGGER_ZONE_STAGE,

	MVTRIGGER_TELEPORT,
	MVTRIGGER_MULTI_BHOP,
	MVTRIGGER_SINGLE_BHOP,
	MVTRIGGER_SEQUENTIAL_BHOP,
	MVTRIGGER_COUNT,
};

struct MvTrigger
{
	MvTriggerType type;
	CEntityHandle entity;

	// MVTRIGGER_MODIFIER
	struct Modifier
	{
		bool disablePausing;
		bool disableCheckpoints;
		bool disableTeleports;
		bool disableJumpstats;
		bool enableSlide;
	} modifier;

	// MVTRIGGER_ANTI_BHOP
	struct Antibhop
	{
		f32 time;
	} antibhop;

	// MVTRIGGER_ZONE_*
	struct Zone
	{
		char courseDescriptor[128];
		i32 courseIndex; // determined at runtime
		// only used with stage zone!
		i32 stageNumber;
	} zone;

	struct Teleport
	{
		char destination[128];
		f32 delay;
		bool useDestinationAngles;
		bool resetSpeed;
		bool reorientPlayer;
		bool relative;
	} teleport;
};

struct MvCourseDescriptor
{
	char name[128];
	bool disableCheckpoints;
	CEntityHandle entity;
};

internal struct
{
	i32 triggerCount;
	MvTrigger triggers[2048];
	i32 courseCount;
	MvCourseDescriptor courses[512];
} g_mappingApi;

internal MvTrigger *Mapi_NewTrigger(MvTriggerType type)
{
	// TODO: bounds checking
	assert(g_mappingApi.triggerCount < Q_ARRAYSIZE(g_mappingApi.triggers));
	MvTrigger *result = nullptr;
	if (g_mappingApi.triggerCount < Q_ARRAYSIZE(g_mappingApi.triggers))
	{
		result = &g_mappingApi.triggers[g_mappingApi.triggerCount++];
		*result = {};
		result->type = type;
	}
	return result;
}

internal MvCourseDescriptor *Mapi_NewCourse()
{
	// TODO: bounds checking
	assert(g_mappingApi.courseCount < Q_ARRAYSIZE(g_mappingApi.courses));
	MvCourseDescriptor *result = nullptr;
	if (g_mappingApi.courseCount < Q_ARRAYSIZE(g_mappingApi.courses))
	{
		result = &g_mappingApi.courses[g_mappingApi.courseCount++];
		*result = {};
	}
	return result;
}

internal void Mapi_OnTriggerMultipleSpawn(const EntitySpawnInfo_t *info)
{
	const CEntityKeyValues *ekv = info->m_pKeyValues;
	MvTriggerType type = (MvTriggerType)ekv->GetInt(KEY_TRIGGER_TYPE, MVTRIGGER_DISABLED);

	if (type <= MVTRIGGER_DISABLED || type >= MVTRIGGER_COUNT)
	{
		// TODO: print error!
		assert(0);
		return;
	}
	MvTrigger *trigger = Mapi_NewTrigger(type);

	if (!trigger)
	{
		// TODO: print error!
		assert(0);
		return;
	}

	trigger->entity = info->m_pEntity->GetRefEHandle();
	switch (type)
	{
		case MVTRIGGER_MODIFIER:
		{
			trigger->modifier.disablePausing = ekv->GetBool("timer_modifier_disable_pause");
			trigger->modifier.disableCheckpoints = ekv->GetBool("timer_modifier_disable_checkpoints");
			trigger->modifier.disableTeleports = ekv->GetBool("timer_modifier_disable_teleports");
			trigger->modifier.disableJumpstats = ekv->GetBool("timer_modifier_disable_jumpstats");
			trigger->modifier.enableSlide = ekv->GetBool("timer_modifier_enable_slide");
		}
		break;

		case MVTRIGGER_ANTI_BHOP:
		{
			trigger->antibhop.time = ekv->GetFloat("timer_anti_bhop_time");
		}
		break;

		case MVTRIGGER_ZONE_START:
		case MVTRIGGER_ZONE_END:
		case MVTRIGGER_ZONE_SPLIT:
		case MVTRIGGER_ZONE_CHECKPOINT:
		case MVTRIGGER_ZONE_STAGE:
		{
			// TODO: find course descriptor entity!
			const char *courseDescriptor = ekv->GetString("timer_zone_course_descriptor");
			V_snprintf(trigger->zone.courseDescriptor, sizeof(trigger->zone.courseDescriptor), "%s", courseDescriptor);

			if (type == MVTRIGGER_ZONE_STAGE)
			{
				trigger->zone.stageNumber = ekv->GetInt("timer_zone_stage_number", INVALID_STAGE_NUMBER);
			}
			else
			{
				trigger->zone.stageNumber = INVALID_STAGE_NUMBER;
			}
		}
		break;

		case MVTRIGGER_TELEPORT:
		case MVTRIGGER_MULTI_BHOP:
		case MVTRIGGER_SINGLE_BHOP:
		case MVTRIGGER_SEQUENTIAL_BHOP:
		{
			const char *destination = ekv->GetString("timer_teleport_destination");
			V_snprintf(trigger->teleport.destination, sizeof(trigger->teleport.destination), "%s", destination);
			trigger->teleport.delay = ekv->GetFloat("timer_anti_bhop_time", -1.0f);
			trigger->teleport.useDestinationAngles = ekv->GetBool("timer_teleport_use_dest_angles");
			trigger->teleport.resetSpeed = ekv->GetBool("timer_teleport_reset_speed");
			trigger->teleport.reorientPlayer = ekv->GetBool("timer_teleport_reorient_player");
			trigger->teleport.relative = ekv->GetBool("timer_teleport_relative");
		}
		break;

		// case MVTRIGGER_RESET_CHECKPOINTS:
		// case MVTRIGGER_SINGLE_BHOP_RESET:
		default:
			break;
	}
}

internal void Mapi_OnInfoTargetSpawn(const EntitySpawnInfo_t *info)
{
	const CEntityKeyValues *ekv = info->m_pKeyValues;

	if (!ekv->GetBool(KEY_IS_COURSE_DESCRIPTOR))
	{
		return;
	}

	MvCourseDescriptor *course = Mapi_NewCourse();
	if (!course)
	{
		// TODO: error
		return;
	}

	course->entity = info->m_pEntity->GetRefEHandle();
	const char *name = ekv->GetString("timer_course_name");
	V_snprintf(course->name, sizeof(course->name), "%s", name);
	course->disableCheckpoints = ekv->GetBool("timer_course_disable_checkpoint");
}

internal MvTrigger *FindMvTrigger(CBaseTrigger *trigger)
{
	MvTrigger *result = nullptr;
	if (!trigger->m_pEntity)
	{
		return result;
	}

	CEntityHandle triggerHandle = trigger->GetRefEHandle();
	if (!trigger || !triggerHandle.IsValid() || trigger->m_pEntity->m_flags & EF_IS_INVALID_EHANDLE)
	{
		return result;
	}

	for (i32 i = 0; i < g_mappingApi.triggerCount; i++)
	{
		if (triggerHandle == g_mappingApi.triggers[i].entity)
		{
			result = &g_mappingApi.triggers[i];
			break;
		}
	}

	return result;
}

bool mappingapi::IsTriggerATimerZone(CBaseTrigger *trigger)
{
	MvTrigger *mvTrigger = FindMvTrigger(trigger);
	if (!mvTrigger)
	{
		return false;
	}
	static_assert(MVTRIGGER_ZONE_START == 5 && MVTRIGGER_ZONE_STAGE == 9,
				  "Don't forget to change this function when changing the MvTriggerType enum!!!");
	return mvTrigger->type >= MVTRIGGER_ZONE_START && mvTrigger->type <= MVTRIGGER_ZONE_STAGE;
}

void mappingapi::Initialize()
{
	g_mappingApi.triggerCount = 0;
	g_mappingApi.courseCount = 0;
}

void mappingapi::OnTriggerMultipleStartTouchPost(KZPlayer *player, CBaseTrigger *trigger)
{
	MvTrigger *touched = FindMvTrigger(trigger);
	if (!touched)
	{
		return;
	}

	switch (touched->type)
	{
		case MVTRIGGER_ZONE_START:
		{
			player->StartZoneStartTouch();
		}
		break;

		case MVTRIGGER_ZONE_END:
		{
			player->EndZoneStartTouch();
		}
		break;
		// TODO:
		case MVTRIGGER_ZONE_SPLIT:
		case MVTRIGGER_ZONE_CHECKPOINT:
		case MVTRIGGER_ZONE_STAGE:
		default:
			break;
	}
}

void mappingapi::OnTriggerMultipleEndTouchPost(KZPlayer *player, CBaseTrigger *trigger)
{
	MvTrigger *touched = FindMvTrigger(trigger);
	if (!touched)
	{
		return;
	}

	switch (touched->type)
	{
		case MVTRIGGER_ZONE_START:
		{
			player->StartZoneEndTouch();
		}
		break;

		// TODO:
		case MVTRIGGER_ZONE_SPLIT:
		case MVTRIGGER_ZONE_CHECKPOINT:
		case MVTRIGGER_ZONE_STAGE:
		default:
			break;
	}
}

void mappingapi::OnSpawnPost(int count, const EntitySpawnInfo_t *info)
{
	if (!info)
	{
		return;
	}

	for (i32 i = 0; i < count; i++)
	{
		auto ekv = info[i].m_pKeyValues;
		if (!info[i].m_pEntity || !ekv || !info[i].m_pEntity->GetClassname())
		{
			continue;
		}
		const char *classname = info[i].m_pEntity->GetClassname();
		Msg("spawned classname %s\n", classname);
		if (V_stricmp(classname, "trigger_multiple") == 0)
		{
			Mapi_OnTriggerMultipleSpawn(&info[i]);
		}
		else if (V_stricmp(classname, "info_target_server_only") == 0)
		{
			Mapi_OnInfoTargetSpawn(&info[i]);
		}
	}
}
