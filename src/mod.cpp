#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_midna.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include <mods/svc/ui.h>

DEFINE_MOD();
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(ConfigService, svc_config);

DEFINE_HOOK(&daAlink_c::procMoveInit, LinkProcMoveInit);
DEFINE_HOOK(&daAlink_c::setDoubleAnime, LinkSetDoubleAnime);
DEFINE_HOOK(&daAlink_c::checkNormalAction, LinkCheckCutAction);
DEFINE_HOOK(&daAlink_c::decideCommonDoStatus, LinkDecideCommonDoStatus);

// NEW: hooks for the held Wolf sprint
DEFINE_HOOK(&daAlink_c::procWolfMove, WolfSprintMove);
DEFINE_HOOK(&daAlink_c::setWolfAnmVoice, WolfSprintVoiceAnm);
DEFINE_HOOK(&daMidna_c::execute, WolfSprintMidnaExecute);

UiElementHandle statusText1 = 0;
UiElementHandle statusText2 = 0;
UiElementHandle statusText3 = 0;
ConfigVarHandle var1 = 0;
ConfigVarHandle var2 = 0;
ConfigVarHandle var3 = 0;

bool running = false;
bool holdingA = false;

// ---------------------------------------------------------------------------
// NEW: Wolf sprint (hold A as Wolf Link to keep the dash going).
// Adapted from the Wolf sprint logic in Twilit Essentials, without stamina.
// ---------------------------------------------------------------------------
static constexpr int kWolfBurstIntervalFrames = 90;
static constexpr int kWolfMinRunFrames = 6;
static constexpr u8 kWolfVoiceDash = 4;

static int s_wolfBurstTimer = 0;
static bool s_wolfWasSprinting = false;
static int s_wolfRunFrames = 0;
static bool s_wolfMuteDashVoice = false;

static bool wolf_sprint_wanted(daAlink_c* link) {
    if (!link || !link->mpHIO) return false;
    if (dComIfGp_isPauseFlag()) return false;
    if (link->checkEventRun()) return false;
    if (mDoCPd_c::getHoldA(0) == 0) return false;
    return true;
}

static void wolf_apply_dash_speed(daAlink_c* link) {
    const daAlinkHIO_wlMove_c1& wl = link->mpHIO->mWolf.mWlMove.m;
    f32 dashMax;
    if (link->checkWolfSlowDash()) {
        dashMax = wl.mADashMaxSpeedSlow;
    } else if (link->field_0x2fc7 == 2) {
        dashMax = wl.mADashMaxSpeedSlow2;
    } else {
        dashMax = wl.mADashMaxSpeed;
    }
    link->mMaxSpeed = dashMax;
}

static void wolf_top_up_dash_duration(daAlink_c* link) {
    const daAlinkHIO_wlMove_c1& wl = link->mpHIO->mWolf.mWlMove.m;
    if (link->checkWolfSlowDash()) {
        link->field_0x30d0 = wl.mADashDurationSlow;
    } else if (link->field_0x2fc7 == 2) {
        link->field_0x30d0 = wl.mADashDurationSlow2;
    } else {
        link->field_0x30d0 = wl.mADashDuration;
    }
}

static HookAction wolf_move_pre(ModContext*, void* args, void* retval, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    if (!wolf_sprint_wanted(link)) {
        s_wolfBurstTimer = 0;
        s_wolfMuteDashVoice = false;
        if (s_wolfWasSprinting && s_wolfRunFrames >= kWolfMinRunFrames && link && link->mpHIO) {
            link->field_0x30d0 = 0;
            link->offNoResetFlg1(daPy_py_c::FLG1_DASH_MODE);
            const f32 runMax = link->mpHIO->mWolf.mWlMoveNoP.m.mMaxSpeed;
            if (link->mNormalSpeed > runMax) link->mNormalSpeed = runMax;
        }
        s_wolfWasSprinting = false;
        s_wolfRunFrames = 0;
        return HOOK_CONTINUE;
    }

    const bool wasSprinting = s_wolfWasSprinting;
    s_wolfWasSprinting = true;
    s_wolfRunFrames++;
    link->onNoResetFlg1(daPy_py_c::FLG1_DASH_MODE);
    wolf_top_up_dash_duration(link);
    wolf_apply_dash_speed(link);
    if (!wasSprinting) {
        link->mNormalSpeed = link->mMaxSpeed;
    }

    if (++s_wolfBurstTimer < kWolfBurstIntervalFrames) return HOOK_CONTINUE;
    s_wolfBurstTimer = 0;
    s_wolfMuteDashVoice = true;

    link->procWolfDashInit();
    wolf_apply_dash_speed(link);
    if (retval) *static_cast<int*>(retval) = 1;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction wolf_voice_anm_pre(ModContext*, void* args, void*, void*) {
    if (!args) return HOOK_CONTINUE;
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link != nullptr && link->field_0x2fd8 == kWolfVoiceDash && s_wolfMuteDashVoice &&
        wolf_sprint_wanted(link)) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

static void wolf_midna_execute_post(ModContext*, void* args, void*, void*) {
    if (!args) return;
    daMidna_c* midna = mods::arg<daMidna_c*>(args, 0);
    if (midna == nullptr || midna->mSoundID != Z2SE_MDN_V_CLINGST || midna->mVoiceFrame < 0.0f) {
        return;
    }
    daAlink_c* link = static_cast<daAlink_c*>(daPy_getLinkPlayerActorClass());
    if (!s_wolfMuteDashVoice || link == nullptr || !link->checkWolf() ||
        !wolf_sprint_wanted(link)) {
        return;
    }
    midna->mVoiceFrame = -1.0f;
}

extern "C" {

HookAction link_proc_move_init_pre(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (!running && link && link->mProcID == daAlink_c::daAlink_PROC::PROC_FRONT_ROLL) {
        // CHANGED: was getHoldA, now the sprint button is R
        if (mDoCPd_c::getHoldR(0) != 0 && !link->checkEventRun() && !link->checkBootsOrArmorHeavy())
        {
            if (link->mEquipItem != 0xFF) {
                link->allUnequip(0);
            }

            running = true;
            holdingA = true;

            // Set here to prevent roll briefly appearing when in non-toggle mode
            link->setDoStatus(BUTTON_STATUS_NONE);

            dCamera_c* camera = dCam_getBody();
            if (camera) {
                camera->mCamParam.mManualMode = 0;
            }
        }
    }
    return HOOK_CONTINUE;
}

HookAction link_set_double_anime_pre(ModContext* ctx, void* args, void*, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link && running) {
        daAlink_c::daAlink_ANM& linkAnm = mods::arg_ref<daAlink_c::daAlink_ANM>(args, 5);

        if (linkAnm == link->ANM_RUN) {
            int64_t speed = 100;
            svc_config->get_int(mod_ctx, var1, &speed);
            f32& anmSpeed = mods::arg_ref<f32>(args, 3);
            anmSpeed = 2.0f * (speed / 100.0f);
            linkAnm = link->ANM_RUN_B;
            link->mNormalSpeed = 35.0f * (speed / 100.0f);
        }
    }
    return HOOK_CONTINUE;
}

HookAction link_check_cut_action_pre(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link && running && link->swordSwingTrigger()) {
        u16 equipSword = dComIfGs_getSelectEquipSword();

        bool allowNoSword = false;
        svc_config->get_bool(mod_ctx, var2, &allowNoSword);
        if (!allowNoSword && equipSword == 0xFF) {
            return HOOK_CONTINUE;
        }

        if (!link->checkSwordEquipAnime() && equipSword != 0xFF) {
            if (link->checkWoodSwordEquip()) {
                link->seStartSwordCut(Z2SE_AL_ITEM_TAKEOUT_FAST);
            } else {
                link->seStartSwordCut(Z2SE_AL_SWORD_PULLOUT);
            }
        }

        link->mEquipItem = 0x103;
        link->setItemModel();
        link->procCutJumpInit(FALSE);
        running = false;

        if (retval != nullptr) {
            *static_cast<int*>(retval) = 1;
        }

        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void link_decide_common_do_status_post(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();

    bool toggle = false;
    svc_config->get_bool(mod_ctx, var3, &toggle);

    // BUTTON_STATUS_UNK_121 is Roll
    if (dComIfGp_getDoStatus() == BUTTON_STATUS_UNK_121)
    {
        if (!toggle)
        {
            if (running)
            {
                link->setDoStatus(BUTTON_STATUS_NONE);
            }
        }
        else
        {
            if (running)
            {
                link->setDoStatus(BUTTON_STATUS_CANCEL);
            }
            else if (holdingA)
            {
                link->setDoStatus(BUTTON_STATUS_NONE);
            }
        }
    }
}

ModResult build(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Settings");

    UiControlDesc control1 = UI_CONTROL_DESC_INIT;
    control1.kind = UI_CONTROL_NUMBER;
    control1.label = "Speed Multiplier (%)";
    control1.help_rml = "The % of speed at which Link sprints at. Default is 100%.";
    control1.binding = UI_BINDING_CONFIG_VAR;
    control1.config_var = var1;  // from svc_config->register_var
    control1.min = 80;
    control1.max = 200;
    control1.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &control1, &statusText1);

    UiControlDesc control2 = UI_CONTROL_DESC_INIT;
    control2.kind = UI_CONTROL_TOGGLE;
    control2.label = "Allow Jump Slash Without Sword";
    control2.help_rml = "Allow Jump Slashing even if you don't have a sword equipped.";
    control2.binding = UI_BINDING_CONFIG_VAR;
    control2.config_var = var2;  // from svc_config->register_var
    svc_ui->pane_add_control(mod_ctx, panel, &control2, &statusText2);

    UiControlDesc control3 = UI_CONTROL_DESC_INIT;
    control3.kind = UI_CONTROL_TOGGLE;
    control3.label = "Toggle Sprint";
    control3.help_rml = "Shown in the help pane while focused.";
    control3.binding = UI_BINDING_CONFIG_VAR;
    control3.config_var = var3;  // from svc_config->register_var
    svc_ui->pane_add_control(mod_ctx, panel, &control3, &statusText3);

    return MOD_OK;
}

ModResult update(ModContext*, void*, ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_initialize(ModError*) {
    ConfigVarDesc desc1 = CONFIG_VAR_DESC_INIT;
    desc1.name = "speedMultiplier";
    desc1.type = CONFIG_VAR_INT;
    desc1.default_int = 100;
    svc_config->register_var(mod_ctx, &desc1, &var1);

    ConfigVarDesc desc2 = CONFIG_VAR_DESC_INIT;
    desc2.name = "allowWithoutSword";
    desc2.type = CONFIG_VAR_BOOL;
    desc2.default_bool = false;
    svc_config->register_var(mod_ctx, &desc2, &var2);

    ConfigVarDesc desc3 = CONFIG_VAR_DESC_INIT;
    desc3.name = "toggleSprint";
    desc3.type = CONFIG_VAR_BOOL;
    desc3.default_bool = false;
    svc_config->register_var(mod_ctx, &desc3, &var3);

    mods::hook::add_pre<LinkProcMoveInit>(link_proc_move_init_pre);
    mods::hook::add_pre<LinkSetDoubleAnime>(link_set_double_anime_pre);
    mods::hook::add_pre<LinkCheckCutAction>(link_check_cut_action_pre);
    mods::hook::add_post<LinkDecideCommonDoStatus>(link_decide_common_do_status_post);

    // NEW: Wolf sprint hooks
    mods::hook::add_pre<WolfSprintMove>(wolf_move_pre);
    mods::hook::add_pre<WolfSprintVoiceAnm>(wolf_voice_anm_pre);
    mods::hook::add_post<WolfSprintMidnaExecute>(wolf_midna_execute_post);

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build;
    panel.update = update;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    return MOD_OK;
}

// CHANGED: human sprint now starts by holding R while moving (no roll needed)
// and stops when R is released. The "Toggle Sprint" option no longer applies.
MOD_EXPORT ModResult mod_update(ModError*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (!link) {
        running = false;
        return MOD_OK;
    }

    bool holdR = mDoCPd_c::getHoldR(0) != 0;
    bool inMove = link->mProcID == daAlink_c::daAlink_PROC::PROC_MOVE;

    if (!running) {
        if (holdR && inMove && mDoCPd_c::getStickValue(0) != 0 &&
            !link->checkEventRun() && !link->checkBootsOrArmorHeavy())
        {
            if (link->mEquipItem != 0xFF) {
                link->allUnequip(0);
            }
            running = true;

            dCamera_c* camera = dCam_getBody();
            if (camera) {
                camera->mCamParam.mManualMode = 0;
            }
        }
    } else if (!holdR || !inMove || link->checkEventRun()) {
        running = false;
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
