/*
    RE1 Quick Knife Mod - Prototype

    Hold Right Bumper: equip knife from hidden slot, auto-aim.
    Release:           restore previous weapon, stop aiming.

    Key addresses (RE1 PC Classic REBirth):
      G_BASE      = 0xC33090
      G_KEY       = G_BASE + 0x5680  (0xC38710)
      G_KEY_TRG   = G_BASE + 0x5682  (0xC38712)
      PL_EQUIP_ID = G_BASE + 0x2126  (0xC351B6)  equipped item ID
      Item_work   = 0xC38814                      player inventory (2 bytes/slot)

    Item IDs (from RE1-Mod-SDK item.xml):
      0 = nothing, 1 = Combat Knife, 2 = Beretta, 3 = Shotgun ...

    Inventory slots:
      Jill : 0-7 visible, 8-11 hidden
      Chris: 0-5 visible, 6-11 hidden
      Slot 11 used as permanent hidden knife slot.

    Hook strategy:
      Mid-hook at 0x483527 (G_KEY write site in the pad function).
      Stub layout: [original bytes] pushfd pushad CALL QuickKnifeInjectFn popad popfd JMP back
      Our inject function runs synchronously after the game writes G_KEY/G_KEY_TRG,
      so KEY_AIM is guaranteed to be set when game input logic reads those values.
*/

#include "framework.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <intrin.h>
#include <ddraw.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed   int   s32;

/* ================================================================
   LOGGING
   ================================================================ */
static FILE* g_log = NULL;

static void LogInit(void)
{
    fopen_s(&g_log, "quick_knife.log", "w");
    if (g_log)
    {
        fprintf(g_log, "=== Quick Knife Mod Log ===\n\n");
        fflush(g_log);
    }
}

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fprintf(g_log, "\n");
    fflush(g_log);
}

static void LogClose(void)
{
    if (g_log)
    {
        fprintf(g_log, "\n=== End of log ===\n");
        fclose(g_log);
        g_log = NULL;
    }
}

/* ================================================================
   XINPUT
   ================================================================ */
#pragma pack(push, 1)
typedef struct {
    WORD  wButtons;
    BYTE  bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY;
    SHORT sThumbRX, sThumbRY;
} MY_XINPUT_GAMEPAD;

typedef struct {
    DWORD             dwPacketNumber;
    MY_XINPUT_GAMEPAD Gamepad;
} MY_XINPUT_STATE;
#pragma pack(pop)

typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, MY_XINPUT_STATE*);
static XInputGetStateFn g_XInputGetState = NULL;

/* ================================================================
   GAME MEMORY
   ================================================================ */
#define G_BASE        0xC33090U
#define G_KEY         ((volatile u16*)(G_BASE + 0x5680))
#define G_KEY_TRG     ((volatile u16*)(G_BASE + 0x5682))
#define PL_EQUIP_ID    ((volatile u8*) (G_BASE + 0x2126))   /* 0xC351B6 — derived each frame from PL_ACTIVE_SLOT */
#define PL_ROUTINE_1   ((volatile u8*) (G_BASE + 0x21A9))   /* 0xC35239 */
#define PL_ACTIVE_SLOT ((volatile u8*) 0xC38719U)           /* 1-based active slot index, drives PL_EQUIP_ID */
#define G_ITEM_WORK    ((volatile u8*) 0xC38814U)            /* Item_work[0] */

#define KEY_READY     0x0100
#define KEY_AIM       0x0400
#define PAD_BTN_RB    0x0200U
#define PAD_BTN_Y     0x8000U

/* Debug menu string mirrors found in CE. Diagnostic only. */
#define DEBUG_KEY_STR1 ((volatile const char*)0x00F8FC4AU)
#define DEBUG_KEY_STR2 ((volatile const char*)0x00F8FC50U)
#define DEBUG_KEY_STR3 ((volatile const char*)0x00F8FC54U)
#define DEBUG_KEY_STR4 ((volatile const char*)0x00F8FC5AU)

#define KNIFE_ID      1
#define KNIFE_SLOT    11

#define HOOK_SITE     ((u8*)0x483527)

/* Safe weapon animation/sound setup — can be called from mid-hook without crashing.
   Sets weapon audio/animation state (not visual model). */
typedef void (__cdecl* LoadWeaponFn)(u32 item_id, u32 table_ptr);
#define LOAD_WEAPON_FN     ((LoadWeaponFn)0x00467530)
#define WEAPON_TABLE_PTR   0x00D4117CU
#define WEAPON_DIRTY_FLAG  ((volatile u8*)0xC2E93EU)

/* Full weapon model/animation loader — UNSAFE from mid-hook, only safe in player-update context.
   Called as: LoadWeaponModel(item_id, 0x0E, 0x00C5A890, 0x00C62290) */
typedef void (__cdecl* LoadWeaponModelFn)(u32 item_id, u32 arg1, u32 table1, u32 table2);
#define LOAD_WEAPON_MODEL_FN  ((LoadWeaponModelFn)0x00414730)
#define WEAPON_MODEL_ARG1     0x0EU
#define WEAPON_MODEL_TABLE1   0x00C5A890U
#define WEAPON_MODEL_TABLE2   0x00C62290U
#define WEAPON_ANIM_BUFFER_SIZE 0x400U

/* FUN_00437800 — called by LWM to set up per-weapon rendering state.
   Args: (pSin[14]+0x0C, *(pSin[14]+0x18)) = (dest_struct_addr, WEAPON_MODEL_TABLE2_addr).
   Safety unknown — trying per-frame call from hook to re-trigger rendering with knife values. */
typedef void (__cdecl* SetupWeaponRenderFn)(u32 dest, u32 table_ptr);
#define FUN_00437800  ((SetupWeaponRenderFn)0x00437800U)

/* FUN_00429040 case 2 CALL site — kept for reference (intercept confirmed non-firing).
   Case 2 appears unreachable via normal dispatch path (range check or outer guard). */
#define DISPATCH_CASE2_SITE   ((u8*)0x004290AAU)
#define WEAPON_ANIM_PTR       ((volatile u32*)0xC35310U)   /* secondary animation ptr, used by handlers */

/* Weapon model pointers — set by LoadWeaponModel_maybe (0x414730).
   Values captured from CE with knife equipped from inventory:
     0xC35314 = current weapon model ptr (knife: 0x00C5F1F8, pistol: 0x00C601D0)
     table entry at pSin_parts+14*124+0x14 (knife: 0x00C5F50C, pistol: 0x00C605A0) */
#define MODEL_DIRECT_PTR  ((volatile u32*)0xC35314U)
#define PSIN_PARTS_PTR    ((volatile u32*)0xC3524CU)
#define KNIFE_MODEL_PTR1  0x00C5F1F8U
#define KNIFE_MODEL_PTR2  0x00C5F50CU

/* Weapon update dispatcher state machine */
#define DISPATCHER_STATE    ((volatile u8*)0xC3523AU)
#define DISPATCHER_SUBSTATE ((volatile u8*)0xC3523BU)
#define DISPATCHER_MODE40   ((volatile u8*)0xC35240U)
#define DISPATCHER_SEQ71    ((volatile u8*)0xC35271U)
#define DISPATCHER_FRAME72  ((volatile u8*)0xC35272U)
#define DISPATCHER_FRAME73  ((volatile u8*)0xC35273U)
#define DISPATCHER_TIMER76  ((volatile u16*)0xC35276U)
#define DISPATCHER_TIMER78  ((volatile u16*)0xC35278U)

/* Weapon handler dispatch pointer — stored at 0x4AAB04, read by the per-weapon aiming system.
   Pistol: 0x408980 (FUN_00408980).  Knife: 0x409D50 (confirmed via VS debugger).
   Writing this directly forces the per-weapon handler to knife without LoadWeaponModel. */
#define WEAPON_HANDLER_PTR     ((volatile u32*)0x4AAB04U)
#define KNIFE_HANDLER_ADDR     0x00409D50U
#define PISTOL_HANDLER_ADDR    0x00408980U

/* Animation data pointers — read by FUN_00429040 case 0 and passed to FUN_004010D0.
   Must point to the correct weapon's animation data BEFORE the dispatcher runs,
   otherwise the animation remains that of the previous weapon even if the model switches.
   Knife values captured from Codex session log with knife naturally equipped from inventory. */
#define PL_PKAN        ((volatile u32*)0xC35244U)
#define PL_PSEQ        ((volatile u32*)0xC35248U)
#define KNIFE_PKAN     0x00C40090U
#define KNIFE_PSEQ     0x00C4333CU

/* Animation/model commit helper — called from multiple weapon/state handlers.
   Typical weapon-state call pattern is FUN_004010D0(0, ptrA, ptrB, 0x400). */
typedef u32 (__cdecl* CommitAnimFn)(u32 arg0, u32 ptrA, u32 ptrB, u32 size);
#define COMMIT_ANIM_FN   ((CommitAnimFn)0x004010D0U)
#define COMMIT_HOOK_SITE ((u8*)0x004010D0U)

/* ================================================================
   STATE
   ================================================================ */
static volatile BOOL g_running       = FALSE;
static HANDLE        g_thread        = NULL;
static BOOL          g_knifeActive   = FALSE;
static BOOL          g_uiKnifePreviewActive = FALSE;
static BOOL          g_inventoryLatched = FALSE;
static u8            g_savedEquipId  = 0;   /* PL_EQUIP_ID backup */
static u8            g_savedSlot     = 0;   /* PL_ACTIVE_SLOT backup (real restore) */
static u8            g_savedItemId   = 0;   /* item ID in saved slot (for weapon reload) */
static u8            g_savedItemMeta = 0;   /* secondary byte in saved slot (ammo/count/state) */

/* Scheduled one-shot calls via the hook (safe: LOAD_WEAPON_FN at 0x467530 doesn't crash) */
static volatile BOOL g_needEquipLoad   = FALSE;
static volatile BOOL g_needRestoreLoad = FALSE;
static volatile BOOL g_tryLateGameplayLwm = FALSE;
static volatile BOOL g_lateGameplayLwmDone = FALSE;
static volatile BOOL g_tryLateRestoreLwm = FALSE;
static volatile BOOL g_lateRestoreLwmDone = FALSE;
static volatile BOOL g_knifeReleasing = FALSE;  /* RB released mid-swing — draining attack animation */
static int           g_swingDrainFrames = 0;    /* frame counter for swing drain cap */
static volatile int  g_aimBlockFrames   = 0;    /* frames to suppress aim/ready keys after knife restore */
static volatile BOOL g_deferredKnifePress = FALSE;
static volatile BOOL g_restoreWakeTrigSent = FALSE;
static volatile BOOL g_restoreHookWakeTrigSent = FALSE;
static volatile int  g_deferredKnifePressFrames = 0;
static int           g_rbCycleCounter = 0;
static int           g_activeRbCycle = 0;
static int           g_completedRbCycle = 0;
static BOOL          g_postRbObserveArmed = FALSE;
static BOOL          g_postRbStableLogged = FALSE;
static int           g_postRbReadyLogBudget = 0;

/* One-shot dispatch redirect: force DISPATCHER_STATE=0, SUBSTATE=2 so the
   game's own FUN_00429040 case 2 runs on the next dispatch cycle.
   Case 2 calls LoadWeaponModel(PL_EQUIP_ID) in its own legal stack frame,
   then commits with PL_PKAN/PL_PSEQ — the clean knife path.
   This is the only known way to get a legal LWM call. */
static volatile BOOL g_needDispatchRedirect = FALSE;

/* Saved weapon model pointers, captured on RB press and restored on RB release */
static u32 g_savedModelPtr1  = 0;
static u32 g_savedModelPtr2  = 0;
static u32 g_pSinAtSave      = 0;
static u32 g_savedSlot0C     = 0;
static u32 g_savedHandlerPtr  = 0;
static u32 g_savedTbl2        = 0;   /* WEAPON_MODEL_TABLE2 saved at RB press, restored on release */
static u32 g_savedPkan        = 0;   /* PL_PKAN saved at RB press — knife handler overwrites these */
static u32 g_savedPseq        = 0;   /* PL_PSEQ saved at RB press */
static volatile u32 g_knifeModelDynamic = 0;
static u32 g_knifeSlot0C      = 0;   /* pSin[14]+0x0C after knife LWM — rendering state for FUN_00437800 */
static u8  g_savedPsin14[124];       /* full save of pSin[14] (124 bytes) at RB press, before knife LWM */
static u8  g_savedC35271 = 0;        /* C35271 animation sequence selector — saved at RB press */

/* Model-swap: copy knife secondary struct data into pistol secondary slot at RB press.
   Renderer appears to cache the resolved mesh ptr from pSin[14]+0x14 at LWM time —
   changing what pSin[14]+0x14 POINTS TO (without changing the pointer) works around this. */
#define MODEL_SWAP_SIZE 32
static u8 g_pistol2Backup[MODEL_SWAP_SIZE];  /* backup of pistol secondary model struct data */
static u8 g_savedAnimBuffer[WEAPON_ANIM_BUFFER_SIZE];
static u8 g_cachedKnifeAnimBuffer[WEAPON_ANIM_BUFFER_SIZE];
static BOOL g_hasKnifeAnimBuffer = FALSE;

/* Frames to keep calling FUN_00437800 after restore (until game's natural LWM fires). */
static int g_postRestoreFrames = 0;

/* Set TRUE after the first successful inner(knife) preload.
   Prevents our LWM hook from re-running inner(knife) on EVERY subsequent pistol LWM call
   (including the game's natural LWM at ~frame 43 post-restore), which would permanently
   re-contaminate the animation buffer and prevent pistol animations from recovering. */
static BOOL g_preloadDone = FALSE;

/* Full weapon state snapshot — captured after inner(knife) LWM during preload.
   Covers C35200-C35330 (304 bytes): dispatch state, PKAN, PSEQ, model ptrs, handler ptr,
   anim ptrs, and all related side-state in a CONSISTENT knife configuration.
   On RB press we swap in the knife snapshot atomically (all-or-nothing).
   On RB release we swap back the pistol snapshot.
   Previous crashes from individual MODEL_DIRECT_PTR writes happened while dispatch
   was at 12/01 (mid-animation). This snapshot includes dispatch=00/xx, making it
   a consistent init-like state. */
#define WSTATE_BASE   0xC35200U
#define WSTATE_SIZE   0x130U   /* 304 bytes: C35200–C35330 */
static u8  g_knifeWState[WSTATE_SIZE];    /* knife snapshot from preload */
static u8  g_pistolWState[WSTATE_SIZE];   /* pistol snapshot saved on RB press */
static BOOL g_hasKnifeWState = FALSE;

/* LoadWeaponModel entry hook (0x414730).
   Hooks the function itself — fires regardless of indirect/direct call. */
#define LWM_HOOK_SITE ((u8*)0x414730U)
static u8*  g_lwmStub      = NULL;
static u8   g_lwmOrigBytes[16];
static int  g_lwmPatchSize = 0;
static BOOL g_lwmHooked    = FALSE;
static volatile BOOL g_inKnifePreload = FALSE;
typedef void (__cdecl* LWMTrampolineFn)(u32, u32, u32, u32);
static LWMTrampolineFn g_lwmTrampoline = NULL;
static int g_activeLwmLogBudget = 0;

/* FUN_004010D0 entry hook */
static u8*  g_commitStub      = NULL;
static u8   g_commitOrigBytes[16];
static int  g_commitPatchSize = 0;
static BOOL g_commitHooked    = FALSE;
static CommitAnimFn g_commitTrampoline = NULL;
static int g_activeCommitLogBudget = 0;
static int g_naturalCommitLogBudget = 0;
static int g_naturalAttackCommitLogBudget = 0;

/* FUN_0040A670 post-commit stage hook */
typedef void (__cdecl* PostCommitStageFn)(u32 arg0);
#define POST_STAGE_HOOK_SITE ((u8*)0x0040A670U)
static u8*  g_postStageStub      = NULL;
static u8   g_postStageOrigBytes[16];
static int  g_postStagePatchSize = 0;
static BOOL g_postStageHooked    = FALSE;
static PostCommitStageFn g_postStageTrampoline = NULL;
static int g_activePostStageLogBudget = 0;
static int g_naturalPostStageLogBudget = 0;
static int g_preReadyLwmGateLogBudget = 0;
static int g_readyLwmGateLogBudget = 0;
static int g_lateLwmGateLogBudget = 0;
static int g_lateRestoreGateLogBudget = 0;

/* 0x4343A0 present/flip hook — fires after rendering each frame.
   Used to defer dispatch redirect until AFTER render so ENGAGE frame
   doesn't render an invisible weapon. */
typedef void (__cdecl* FlipFn)(void);
#define FLIP_HOOK_SITE ((u8*)0x004343A0U)
static u8*   g_flipStub      = NULL;
static u8    g_flipOrigBytes[16];
static int   g_flipPatchSize = 0;
static BOOL  g_flipHooked    = FALSE;
static FlipFn g_flipTrampoline = NULL;
static volatile BOOL g_pendingDispatchRedirect = FALSE;

/* DirectDraw COM vtable hook.
   Old version only patched one offscreen-surface Flip vtable and never fired in-game.
   New version patches IDirectDraw::CreateSurface plus the Blt/BltFast/Flip slots on any
   candidate surface vtable we can discover, so we can suppress the REAL present path. */
typedef HRESULT (__stdcall* DDSBltFn)(IDirectDrawSurface*, LPRECT, IDirectDrawSurface*, LPRECT, DWORD, LPDDBLTFX);
typedef HRESULT (__stdcall* DDSBltFastFn)(IDirectDrawSurface*, DWORD, DWORD, IDirectDrawSurface*, LPRECT, DWORD);
typedef HRESULT (__stdcall* DDSFlipFn)(IDirectDrawSurface*, IDirectDrawSurface*, DWORD);
typedef HRESULT (__stdcall* DDCreateSurfaceMethodFn)(IDirectDraw*, DDSURFACEDESC*, IDirectDrawSurface**, IUnknown*);

typedef struct DDSurfaceHookEntry
{
    void**       vtable;
    DDSBltFn     origBlt;
    DDSBltFastFn origBltFast;
    DDSFlipFn    origFlip;
    BOOL         active;
} DDSurfaceHookEntry;

#define DD_SURFACE_HOOK_MAX 8
static DDSurfaceHookEntry      g_ddSurfaceHooks[DD_SURFACE_HOOK_MAX];
static DDSFlipFn               g_origDDSFlip = NULL;  /* legacy helper kept for old DDSFlipHook body */
static DDCreateSurfaceMethodFn g_origDDCreateSurface = NULL;
static void**                  g_ddCreateVtable = NULL;
static int                     g_ddCreateLogBudget = 0;
static int                     g_ddPresentLogBudget = 0;
static volatile BOOL g_suppressNextFlip = FALSE;
static volatile int  g_entryFlipHoldBudget = 0;
static volatile int  g_entryFlipSettleBudget = 0;
static volatile int  g_entryFlipLogBudget = 0;
static volatile int  g_restoreFlipHoldBudget = 0;
static volatile int  g_restoreFlipSettleBudget = 0;
static volatile int  g_restoreFlipLogBudget = 0;

static int DeterminePatchSize(u8* t);  /* forward declaration */
static BOOL IsGameplayKnifeVisualReady(void);
static BOOL IsGameplayRestoreVisualReady(void);

static DDSurfaceHookEntry* LookupDDSurfaceHook(IDirectDrawSurface* pSurf)
{
    if (!pSurf)
        return NULL;

    void** vtable = *(void***)pSurf;
    for (int i = 0; i < DD_SURFACE_HOOK_MAX; ++i)
    {
        if (g_ddSurfaceHooks[i].active && g_ddSurfaceHooks[i].vtable == vtable)
            return &g_ddSurfaceHooks[i];
    }
    return NULL;
}

static BOOL TryGetDDSurfaceCaps(IDirectDrawSurface* pSurf, DWORD* capsOut)
{
    if (!pSurf || !capsOut)
        return FALSE;

    DDSCAPS caps;
    ZeroMemory(&caps, sizeof(caps));
    HRESULT hr = pSurf->GetCaps(&caps);
    if (FAILED(hr))
        return FALSE;

    *capsOut = caps.dwCaps;
    return TRUE;
}

static BOOL IsLikelyPresentSurfaceCaps(DWORD caps)
{
    return (caps & DDSCAPS_PRIMARYSURFACE) != 0 ||
           (caps & DDSCAPS_FRONTBUFFER) != 0;
}

static BOOL ShouldSuppressPresentNow(const char* opTag, IDirectDrawSurface* pThis)
{
    BOOL budgetsActive =
        g_suppressNextFlip ||
        g_entryFlipHoldBudget > 0 ||
        g_entryFlipSettleBudget > 0 ||
        g_restoreFlipHoldBudget > 0 ||
        g_restoreFlipSettleBudget > 0;
    int cycleNow = g_knifeActive ? g_activeRbCycle : g_completedRbCycle;
    if (!budgetsActive)
        return FALSE;

    BOOL gameplayKnifeActive = g_knifeActive && !g_inventoryLatched;
    BOOL restoreVisualPending = !g_inventoryLatched &&
                                !g_knifeActive &&
                                (g_restoreFlipHoldBudget > 0 ||
                                 g_restoreFlipSettleBudget > 0 ||
                                 g_needRestoreLoad ||
                                 g_tryLateRestoreLwm ||
                                 !g_lateRestoreLwmDone ||
                                 g_postRestoreFrames > 0);
    if (!gameplayKnifeActive && !restoreVisualPending)
    {
        Log("DDPresentClear: cycle=%d op=%s reason=inactive first=%d eHold=%d eSettle=%d rHold=%d rSettle=%d dispatch=%02X/%02X",
            cycleNow,
            opTag,
            g_suppressNextFlip ? 1 : 0,
            g_entryFlipHoldBudget,
            g_entryFlipSettleBudget,
            g_restoreFlipHoldBudget,
            g_restoreFlipSettleBudget,
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
        g_suppressNextFlip = FALSE;
        g_entryFlipHoldBudget = 0;
        g_entryFlipSettleBudget = 0;
        g_restoreFlipHoldBudget = 0;
        g_restoreFlipSettleBudget = 0;
        return FALSE;
    }

    DWORD caps = 0;
    BOOL haveCaps = TryGetDDSurfaceCaps(pThis, &caps);
    BOOL isPresentSurface = haveCaps && IsLikelyPresentSurfaceCaps(caps);

    if (g_ddPresentLogBudget < 20)
    {
        g_ddPresentLogBudget++;
        Log("DDPresentProbe[%d]: cycle=%d op=%s caps=0x%08X present=%d model=0x%08X tbl2=0x%08X dispatch=%02X/%02X eHold=%d eSettle=%d rHold=%d rSettle=%d first=%d",
            g_ddPresentLogBudget,
            cycleNow,
            opTag,
            caps,
            isPresentSurface ? 1 : 0,
            *MODEL_DIRECT_PTR,
            *(volatile u32*)WEAPON_MODEL_TABLE2,
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            g_entryFlipHoldBudget,
            g_entryFlipSettleBudget,
            g_restoreFlipHoldBudget,
            g_restoreFlipSettleBudget,
            g_suppressNextFlip ? 1 : 0);
    }

    if (!isPresentSurface)
        return FALSE;

    BOOL suppressThisPresent = FALSE;
    u32 modelNow = *MODEL_DIRECT_PTR;
    u32 tbl2Now = *(volatile u32*)WEAPON_MODEL_TABLE2;
    BOOL knifeVisualReady = IsGameplayKnifeVisualReady();
    BOOL restoreVisualReady = IsGameplayRestoreVisualReady();

    if (g_suppressNextFlip)
    {
        g_suppressNextFlip = FALSE;
        suppressThisPresent = TRUE;
    }

    if (g_entryFlipHoldBudget > 0)
    {
        if (!knifeVisualReady)
        {
            suppressThisPresent = TRUE;
            g_entryFlipHoldBudget--;

            if (g_entryFlipLogBudget < 12)
            {
                g_entryFlipLogBudget++;
                Log("DDPresentHold[%d]: op=%s waiting model=0x%08X saved=0x%08X tbl2=0x%08X dispatch=%02X/%02X budget=%d",
                    g_entryFlipLogBudget,
                    opTag,
                    modelNow, g_savedModelPtr1, tbl2Now,
                    *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                    g_entryFlipHoldBudget);
            }
        }
        else
        {
            if (g_entryFlipSettleBudget == 0)
            {
                g_entryFlipSettleBudget = 2;
                if (g_entryFlipLogBudget < 12)
                {
                    g_entryFlipLogBudget++;
                    Log("DDPresentHold[%d]: op=%s knife visual ready model=0x%08X tbl2=0x%08X dispatch=%02X/%02X",
                        g_entryFlipLogBudget,
                        opTag,
                        modelNow, tbl2Now,
                        *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
                }
            }

            if (g_entryFlipSettleBudget > 0)
            {
                suppressThisPresent = TRUE;
                g_entryFlipSettleBudget--;
            }

            if (g_entryFlipSettleBudget == 0)
                g_entryFlipHoldBudget = 0;
        }
    }
    else if (g_entryFlipSettleBudget > 0)
    {
        suppressThisPresent = TRUE;
        g_entryFlipSettleBudget--;
    }

    if (g_restoreFlipHoldBudget > 0)
    {
        if (!restoreVisualReady)
        {
            suppressThisPresent = TRUE;
            g_restoreFlipHoldBudget--;

            if (g_restoreFlipLogBudget < 12)
            {
                g_restoreFlipLogBudget++;
                Log("DDPresentRestoreHold[%d]: op=%s waiting model=0x%08X saved=0x%08X tbl2=0x%08X savedTbl2=0x%08X dispatch=%02X/%02X budget=%d",
                    g_restoreFlipLogBudget,
                    opTag,
                    modelNow, g_savedModelPtr1, tbl2Now, g_savedTbl2,
                    *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                    g_restoreFlipHoldBudget);
            }
        }
        else
        {
            if (g_restoreFlipSettleBudget == 0)
            {
                g_restoreFlipSettleBudget = 2;
                if (g_restoreFlipLogBudget < 12)
                {
                    g_restoreFlipLogBudget++;
                    Log("DDPresentRestoreHold[%d]: op=%s pistol visual ready model=0x%08X tbl2=0x%08X dispatch=%02X/%02X",
                        g_restoreFlipLogBudget,
                        opTag,
                        modelNow, tbl2Now,
                        *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
                }
            }

            if (g_restoreFlipSettleBudget > 0)
            {
                suppressThisPresent = TRUE;
                g_restoreFlipSettleBudget--;
            }

            if (g_restoreFlipSettleBudget == 0)
                g_restoreFlipHoldBudget = 0;
        }
    }
    else if (g_restoreFlipSettleBudget > 0)
    {
        suppressThisPresent = TRUE;
        g_restoreFlipSettleBudget--;
    }

    if (suppressThisPresent)
    {
        Log("DDPresentSuppress: cycle=%d op=%s caps=0x%08X model=0x%08X tbl2=0x%08X dispatch=%02X/%02X eHold=%d eSettle=%d rHold=%d rSettle=%d",
            cycleNow,
            opTag,
            caps,
            modelNow, tbl2Now,
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            g_entryFlipHoldBudget,
            g_entryFlipSettleBudget,
            g_restoreFlipHoldBudget,
            g_restoreFlipSettleBudget);
    }

    return suppressThisPresent;
}

static BOOL IsGameplayKnifeVisualReady(void)
{
    u32 modelNow = *MODEL_DIRECT_PTR;
    u32 tbl2Now = *(volatile u32*)WEAPON_MODEL_TABLE2;

    return (*PL_EQUIP_ID == KNIFE_ID) &&
           (modelNow != 0) &&
           (modelNow != g_savedModelPtr1) &&
           (tbl2Now == KNIFE_MODEL_PTR2);
}

static BOOL IsGameplayRestoreVisualReady(void)
{
    u32 modelNow = *MODEL_DIRECT_PTR;
    u32 tbl2Now = *(volatile u32*)WEAPON_MODEL_TABLE2;
    u32 handlerNow = *WEAPON_HANDLER_PTR;

    return (*PL_EQUIP_ID == g_savedEquipId) &&
           (*PL_ACTIVE_SLOT == g_savedSlot) &&
           (modelNow == g_savedModelPtr1) &&
           (tbl2Now == g_savedTbl2) &&
           (handlerNow == g_savedHandlerPtr);
}

static void TrySavedRenderRefresh(const char* tag)
{
    if (g_savedSlot0C == 0)
        return;

    FUN_00437800(g_savedSlot0C, WEAPON_MODEL_TABLE2);
    Log("%s: saved render refresh dest=0x%08X tbl2=0x%08X",
        tag, g_savedSlot0C, *(volatile u32*)WEAPON_MODEL_TABLE2);
}

/* FUN_00429040 dispatch[0] entry hook — diagnostic only.
   Confirms whether the game calls dispatch[0] during our 00/02 redirect window. */
#define DISPATCH0_HOOK_SITE ((u8*)0x00429040U)
typedef void (__cdecl* Dispatch0Fn)(void);
static u8*  g_disp0Stub      = NULL;
static u8   g_disp0OrigBytes[16];
static int  g_disp0PatchSize = 0;
static BOOL g_disp0Hooked    = FALSE;
static Dispatch0Fn g_disp0Trampoline = NULL;
static int g_disp0LogBudget = 0;

static void CopyDebugString4(char out[5], volatile const char* src)
{
    for (int i = 0; i < 4; ++i)
    {
        char c = src[i];
        out[i] = (c >= 32 && c <= 126) ? c : '.';
    }
    out[4] = '\0';
}

static void ReadDebugKeyStrings(char a[5], char b[5], char c[5], char d[5])
{
    CopyDebugString4(a, DEBUG_KEY_STR1);
    CopyDebugString4(b, DEBUG_KEY_STR2);
    CopyDebugString4(c, DEBUG_KEY_STR3);
    CopyDebugString4(d, DEBUG_KEY_STR4);
}

static void __cdecl Dispatch0Hook(void)
{
    u8 stateNow = *DISPATCHER_STATE;
    u8 subNow   = *DISPATCHER_SUBSTATE;

    if (g_knifeActive && g_disp0LogBudget < 10)
    {
        g_disp0LogBudget++;
        Log("Dispatch0[%d]: entry state=%02X sub=%02X equip=%d model=0x%08X handler=0x%08X",
            g_disp0LogBudget, stateNow, subNow, *PL_EQUIP_ID,
            *MODEL_DIRECT_PTR, *WEAPON_HANDLER_PTR);
    }

    g_disp0Trampoline();

    if (g_knifeActive && g_disp0LogBudget <= 10)
    {
        Log("Dispatch0[%d]: exit  state=%02X sub=%02X model=0x%08X",
            g_disp0LogBudget, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, *MODEL_DIRECT_PTR);
    }
}

static void InstallDispatch0Hook(void)
{
    u8* site = DISPATCH0_HOOK_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
        { Log("Dispatch0Hook: VirtualProtect failed"); return; }
    if (site[0] == 0xE9)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("Dispatch0Hook: already patched"); return; }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
    {
        /* 0x00429040 currently starts with:
           33 C0          xor eax,eax
           A0 xx xx xx xx mov al,[imm32]
           Hardcode 7 bytes so this diagnostic hook can still install. */
        if (site[0] == 0x33 && site[1] == 0xC0 && site[2] == 0xA0)
        {
            patchSize = 7;
            Log("Dispatch0Hook: fallback patchSize=7 for bytes 0x%02X 0x%02X 0x%02X",
                site[0], site[1], site[2]);
        }
        else
        {
            VirtualProtect(site, 16, oldProt, &oldProt);
            Log("Dispatch0Hook: patchSize=0, bytes=0x%02X 0x%02X 0x%02X", site[0], site[1], site[2]);
            return;
        }
    }

    g_disp0Stub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_disp0Stub)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("Dispatch0Hook: VirtualAlloc failed"); return; }

    memcpy(g_disp0OrigBytes, site, patchSize);
    g_disp0PatchSize = patchSize;

    u8* p = g_disp0Stub;
    memcpy(p, site, patchSize); p += patchSize;
    *p++ = 0xE9;
    *(s32*)p = (s32)((site + patchSize) - (p + 4)); p += 4;
    g_disp0Trampoline = (Dispatch0Fn)g_disp0Stub;

    site[0] = 0xE9;
    *(s32*)(site + 1) = (s32)((u8*)Dispatch0Hook - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_disp0Hooked = TRUE;
    Log("Dispatch0Hook: installed at 0x00429040, patchSize=%d, stub=0x%08X", patchSize, (unsigned)g_disp0Stub);
}

static void UninstallDispatch0Hook(void)
{
    if (!g_disp0Hooked || !g_disp0Stub) return;
    u8* site = DISPATCH0_HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_disp0OrigBytes, g_disp0PatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);
    VirtualFree(g_disp0Stub, 0, MEM_RELEASE);
    g_disp0Stub = NULL;
    g_disp0Hooked = FALSE;
    Log("Dispatch0Hook: uninstalled");
}

/* Safe call-site patch state: logs a live SyncEquip caller
   to verify whether active RB ever enters the legal
   refresh chain before LoadWeaponModel. */
static u8   g_callSiteOrig[5];
static BOOL g_callPatched = FALSE;
#define SYNC_EQUIP_CALL_SITE  ((u8*)0x0040B8C0U)
typedef void (__cdecl* SyncEquipFn)(void);
#define SYNC_EQUIP_FN ((SyncEquipFn)0x0040BB00U)

/* Mid-hook state */
static u8* g_HookStub   = NULL;
static u8  g_OrigBytes[16];
static int g_PatchSize  = 0;
static BOOL g_hookInstalled = FALSE;

/* ================================================================
   INSTRUCTION LENGTH (minimal, handles 0x66-prefixed writes)
   ================================================================ */
static int DeterminePatchSize(u8* t)
{
    int total = 0;
    while (total < 5 && total < 20)
    {
        u8 op = t[total];

        if (op >= 0x50 && op <= 0x5F) { total += 1; continue; }
        if (op == 0x90)               { total += 1; continue; }
        if (op == 0xA1 || op == 0xA3) { total += 5; continue; }
        if (op == 0x83)               { total += 3; continue; }
        if (op == 0x81)               { total += 6; continue; }
        if (op >= 0xB8 && op <= 0xBF) { total += 5; continue; }
        if (op == 0xE8 || op == 0xE9) { total += 5; continue; }

        if (op == 0x66)
        {
            u8 op2 = t[total + 1];
            if (op2 == 0xA1 || op2 == 0xA3) { total += 6; continue; }
            if (op2 >= 0xB8 && op2 <= 0xBF) { total += 4; continue; }
            if (op2 == 0x89 || op2 == 0x8B || op2 == 0xC7)
            {
                u8 modrm = t[total + 2];
                int mod = (modrm >> 6) & 3, rm = modrm & 7;
                int len = 3;
                if (mod == 0 && rm == 5) len += 4;
                else { if (rm == 4) len++; if (mod == 1) len++; else if (mod == 2) len += 4; }
                if (op2 == 0xC7) len += 2;
                total += len; continue;
            }
        }

        if (op == 0x8B || op == 0x89 || op == 0x33 || op == 0x31 || op == 0x85)
        {
            u8 modrm = t[total + 1];
            int mod = (modrm >> 6) & 3, rm = modrm & 7;
            int len = 2;
            if (mod == 0 && rm == 5) len += 4;
            else if (rm == 4) len++;
            if (mod == 1) len++; else if (mod == 2) len += 4;
            total += len; continue;
        }

        break;
    }
    return (total >= 5) ? total : 0;
}

/* ================================================================
   SAFE SYNC-EQUIP CALL-SITE LOGGER
   Replaces a live SyncEquip CALL with a logging shim.
   This tells us whether active RB ever enters the legal
   SyncEquip -> LWM path in the currently active state machine.
   ================================================================ */
static int g_syncEquipLogBudget = 0;

static void __cdecl SafeSyncEquipLogger(void)
{
    u8  beforeEquip = *PL_EQUIP_ID;
    u8  beforeSlot  = *PL_ACTIVE_SLOT;
    u8  beforeDirty = *WEAPON_DIRTY_FLAG;
    u32 beforeModel   = *MODEL_DIRECT_PTR;
    u32 beforeTbl2    = *(volatile u32*)WEAPON_MODEL_TABLE2;
    u32 beforeHandler = *WEAPON_HANDLER_PTR;
    u8  beforeState   = *DISPATCHER_STATE;
    u8  beforeSub     = *DISPATCHER_SUBSTATE;

    SYNC_EQUIP_FN();

    if (g_knifeActive && g_syncEquipLogBudget < 40)
    {
        g_syncEquipLogBudget++;
        Log("SafeSync[%d]: knifeActive=1 slot %u->%u equip %u->%u dirty %02X->%02X dispatch %02X/%02X -> %02X/%02X model 0x%08X->0x%08X tbl2 0x%08X->0x%08X handler 0x%08X->0x%08X",
            g_syncEquipLogBudget,
            beforeSlot, *PL_ACTIVE_SLOT,
            beforeEquip, *PL_EQUIP_ID,
            beforeDirty, *WEAPON_DIRTY_FLAG,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeModel, *MODEL_DIRECT_PTR,
            beforeTbl2, *(volatile u32*)WEAPON_MODEL_TABLE2,
            beforeHandler, *WEAPON_HANDLER_PTR);
    }
}

static void PatchWeaponCallSite(void)
{
    u8* site = SYNC_EQUIP_CALL_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("SafeSync: call patch VirtualProtect failed");
        return;
    }

    memcpy(g_callSiteOrig, site, 5);
    site[0] = 0xE8;
    *(s32*)(site + 1) = (s32)((u8*)SafeSyncEquipLogger - (site + 5));
    VirtualProtect(site, 5, oldProt, &oldProt);
    g_callPatched = TRUE;
    Log("SafeSync: call patch installed at 0x0040B8C0");
}

static void UnpatchWeaponCallSite(void)
{
    if (!g_callPatched) return;

    u8* site = SYNC_EQUIP_CALL_SITE;
    DWORD oldProt;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_callSiteOrig, 5);
    VirtualProtect(site, 5, oldProt, &oldProt);
    g_callPatched = FALSE;
    Log("SafeSync: call patch uninstalled");
}

/* ================================================================
   LOAD WEAPON MODEL HOOK
   Hooks the entry of LoadWeaponModel (0x414730) directly.
   Fires regardless of whether the game calls it via direct or indirect call.
   After the original runs, pre-loads knife model once so animation data
   is in RAM before the player presses RB.
   ================================================================ */
static void __cdecl LoadWeaponModelHook(u32 item_id, u32 arg1, u32 table1, u32 table2)
{
    /* Re-entrancy guard: our pre-load calls the trampoline directly (bypasses this
       hook), but guard anyway in case the game's own code recurses. */
    if (g_inKnifePreload)
    {
        g_lwmTrampoline(item_id, arg1, table1, table2);
        return;
    }

    if (g_knifeActive)
    {
        u8 item8 = (u8)item_id;
        u32 beforeModel   = *MODEL_DIRECT_PTR;
        u32 beforeTbl2    = *(volatile u32*)WEAPON_MODEL_TABLE2;
        u32 beforeHandler = *WEAPON_HANDLER_PTR;

        g_lwmTrampoline(item_id, arg1, table1, table2);

        if (g_activeLwmLogBudget < 40)
        {
            g_activeLwmLogBudget++;
            Log("ActiveLWM[%d]: raw=%u item8=%u arg1=0x%08X slot=%d equip=%d model 0x%08X->0x%08X tbl2 0x%08X->0x%08X handler 0x%08X->0x%08X dispatch=%02X/%02X",
                g_activeLwmLogBudget,
                item_id, item8, arg1, *PL_ACTIVE_SLOT, *PL_EQUIP_ID,
                beforeModel, *MODEL_DIRECT_PTR,
                beforeTbl2, *(volatile u32*)WEAPON_MODEL_TABLE2,
                beforeHandler, *WEAPON_HANDLER_PTR,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
        }
        return;
    }

    if (arg1 != WEAPON_MODEL_ARG1)
    {
        /* Wrong context — just run the trampoline. */
        g_lwmTrampoline(item_id, arg1, table1, table2);
        return;
    }

    if (item_id == KNIFE_ID)
    {
        /* Game loaded knife natively (e.g. inventory equip) while knife not active.
           Let trampoline load the data, then restore all 4 known visual states. */
        u32 savedPtr     = *MODEL_DIRECT_PTR;
        u32 savedHandler = *WEAPON_HANDLER_PTR;
        u32 savedTbl2    = *(volatile u32*)WEAPON_MODEL_TABLE2;
        u32 savedPsinBase = *PSIN_PARTS_PTR;
        u32 savedPsin     = (savedPsinBase > 0x400000u)
                            ? *(volatile u32*)(savedPsinBase + 14u * 124u + 0x14u) : 0;
        g_lwmTrampoline(item_id, arg1, table1, table2);
        if (g_knifeModelDynamic == 0)
            g_knifeModelDynamic = *MODEL_DIRECT_PTR;
        /* Capture rendering state set by FUN_00437800 inside LWM */
        {
            u32 knifePsinBase = *PSIN_PARTS_PTR;
            if (knifePsinBase > 0x400000u)
                g_knifeSlot0C = *(volatile u32*)(knifePsinBase + 14u * 124u + 0x0Cu);
        }
        if (!g_knifeActive)
        {
            /* Passive capture: restore all visual state so pistol keeps rendering. */
            *MODEL_DIRECT_PTR   = savedPtr;
            *WEAPON_HANDLER_PTR = savedHandler;
            *(volatile u32*)WEAPON_MODEL_TABLE2 = savedTbl2;
            if (savedPsinBase > 0x400000u)
                *(volatile u32*)(savedPsinBase + 14u * 124u + 0x14u) = savedPsin;
            Log("LWM: knife passive load -> knifeModel=0x%08X slot0C=0x%08X", g_knifeModelDynamic, g_knifeSlot0C);
        }
        else
        {
            /* Active QK: let dispatch case 2's LWM(knife) take effect immediately.
               Do NOT restore pistol pointers — that was causing the 2-3 frame flicker. */
            Log("LWM: knife ACTIVE load -> knifeModel=0x%08X slot0C=0x%08X model=0x%08X (kept)",
                g_knifeModelDynamic, g_knifeSlot0C, *MODEL_DIRECT_PTR);
        }
    }
    else
    {
        if (!g_preloadDone)
        {
            /* Defer preload until pSin is initialized. At startup, PSIN_PARTS_PTR is 0
               and the knife data at 0xC5F1F8 won't be fully initialized (crashes on use).
               Wait for a later LWM call when pSin is valid. */
            u32 psinNow = *PSIN_PARTS_PTR;
            if (psinNow < 0x400000u)
            {
                Log("LWM: deferring preload — pSin=0x%08X not ready yet", psinNow);
                g_lwmTrampoline(item_id, arg1, table1, table2);
            }
            else
            {
                g_inKnifePreload = TRUE;
                g_lwmTrampoline(KNIFE_ID, arg1, table1, table2);  /* inner: knife first */
                g_inKnifePreload = FALSE;

                g_knifeModelDynamic = *MODEL_DIRECT_PTR;
                {
                    u32 knifePsinBase = *PSIN_PARTS_PTR;
                    if (knifePsinBase > 0x400000u)
                        g_knifeSlot0C = *(volatile u32*)(knifePsinBase + 14u * 124u + 0x0Cu);
                }
                memcpy(g_cachedKnifeAnimBuffer, (const void*)WEAPON_MODEL_TABLE1, WEAPON_ANIM_BUFFER_SIZE);
                g_hasKnifeAnimBuffer = TRUE;

                memcpy(g_knifeWState, (const void*)WSTATE_BASE, WSTATE_SIZE);
                g_hasKnifeWState = TRUE;
                Log("LWM: knife wstate snapshot captured — model=0x%08X slot0C=0x%08X dispatch=%02X/%02X pSin=0x%08X",
                    *MODEL_DIRECT_PTR, g_knifeSlot0C, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, psinNow);

                g_lwmTrampoline(item_id, arg1, table1, table2);  /* outer: weapon last restores all state */

                g_preloadDone = TRUE;
                Log("LWM: pre-loaded knife -> knifeModel=0x%08X slot0C=0x%08X pistolModel=0x%08X",
                    g_knifeModelDynamic, g_knifeSlot0C, *MODEL_DIRECT_PTR);
            }
        }
        else
        {
            /* Preload already done — allow natural weapon LWM to pass through cleanly. */
            g_lwmTrampoline(item_id, arg1, table1, table2);
            Log("LWM: passthrough item=%d (preload done, pistol anim reload)", item_id);
        }
    }
}

static void InstallLWMHook(void)
{
    u8* site = LWM_HOOK_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
        { Log("LWMHook: VirtualProtect failed"); return; }
    if (site[0] == 0xE9)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("LWMHook: already patched"); return; }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("LWMHook: patchSize=0, bytes=0x%02X 0x%02X 0x%02X", site[0], site[1], site[2]); return; }

    g_lwmStub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_lwmStub)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("LWMHook: VirtualAlloc failed"); return; }

    memcpy(g_lwmOrigBytes, site, patchSize);
    g_lwmPatchSize = patchSize;

    /* Trampoline: original prologue bytes + JMP back to site+patchSize.
       Callable as a function — re-enters original code after the patched bytes. */
    u8* p = g_lwmStub;
    memcpy(p, site, patchSize); p += patchSize;
    *p++ = 0xE9;
    *(s32*)p = (s32)((site + patchSize) - (p + 4)); p += 4;
    g_lwmTrampoline = (LWMTrampolineFn)g_lwmStub;

    /* Patch entry: JMP to our hook */
    site[0] = 0xE9;
    *(s32*)(site + 1) = (s32)((u8*)LoadWeaponModelHook - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_lwmHooked = TRUE;
    Log("LWMHook: installed at 0x414730, patchSize=%d, stub=0x%08X", patchSize, (unsigned)g_lwmStub);
}

static void UninstallLWMHook(void)
{
    if (!g_lwmHooked || !g_lwmStub) return;
    u8* site = LWM_HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_lwmOrigBytes, g_lwmPatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);
    VirtualFree(g_lwmStub, 0, MEM_RELEASE);
    g_lwmStub = NULL;
    g_lwmHooked = FALSE;
    Log("LWMHook: uninstalled");
}

/* ================================================================
   COMMIT ANIM HOOK
   Hooks FUN_004010D0 to observe which pointer pair is actually being
   committed during active RB, since active RB does not reach LWM.
   ================================================================ */
static u32 __cdecl CommitAnimHook(u32 arg0, u32 ptrA, u32 ptrB, u32 size)
{
    u32 returnAddr = (u32)_ReturnAddress();
    u32 callSite = (returnAddr >= 5u) ? (returnAddr - 5u) : 0u;
    u8 activeSlot = *PL_ACTIVE_SLOT;
    u8 activeSlotItem = 0;
    BOOL naturalKnifeEquipped = FALSE;
    BOOL naturalKnifeStable = FALSE;
    BOOL naturalKnifeAttackCaller = FALSE;
    if (activeSlot >= 1 && activeSlot <= 12)
        activeSlotItem = G_ITEM_WORK[(activeSlot - 1u) * 2u];
    naturalKnifeEquipped = (!g_knifeActive) &&
                           (*PL_EQUIP_ID == KNIFE_ID) &&
                           (activeSlotItem == KNIFE_ID) &&
                           (activeSlot != (u8)(KNIFE_SLOT + 1));
    naturalKnifeStable = naturalKnifeEquipped;
    naturalKnifeAttackCaller = naturalKnifeEquipped &&
                               (callSite == 0x00409CFAu ||
                                callSite == 0x00409E1Eu ||
                                callSite == 0x00409E82u);

    u8 beforeState = *DISPATCHER_STATE;
    u8 beforeSub   = *DISPATCHER_SUBSTATE;
    u8 beforeMode40 = *DISPATCHER_MODE40;
    u8 beforeSeq71 = *DISPATCHER_SEQ71;
    u8 beforeFrame72 = *DISPATCHER_FRAME72;
    u8 beforeFrame73 = *DISPATCHER_FRAME73;
    u16 beforeTimer76 = *DISPATCHER_TIMER76;
    u16 beforeTimer78 = *DISPATCHER_TIMER78;
    u32 beforeModel = *MODEL_DIRECT_PTR;
    u32 result = g_commitTrampoline(arg0, ptrA, ptrB, size);

    if (g_knifeActive && g_activeCommitLogBudget < 60)
    {
        g_activeCommitLogBudget++;
        Log("Active4010D0[%d]: call=0x%08X ret=0x%08X arg0=0x%08X ptrA=0x%08X ptrB=0x%08X size=0x%08X result=0x%08X dispatch %02X/%02X -> %02X/%02X mode40 %02X->%02X seq71 %02X->%02X frame72 %02X->%02X frame73 %02X->%02X t76 %04X->%04X t78 %04X->%04X equip=%d slot=%d model 0x%08X->0x%08X pkan=0x%08X pseq=0x%08X",
            g_activeCommitLogBudget,
            callSite, returnAddr, arg0, ptrA, ptrB, size, result,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeMode40, *DISPATCHER_MODE40,
            beforeSeq71, *DISPATCHER_SEQ71,
            beforeFrame72, *DISPATCHER_FRAME72,
            beforeFrame73, *DISPATCHER_FRAME73,
            beforeTimer76, *DISPATCHER_TIMER76,
            beforeTimer78, *DISPATCHER_TIMER78,
            *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
            beforeModel, *MODEL_DIRECT_PTR,
            *PL_PKAN, *PL_PSEQ);
    }
    else if (naturalKnifeAttackCaller && g_naturalAttackCommitLogBudget < 40)
    {
        g_naturalAttackCommitLogBudget++;
        Log("NaturalAttack4010D0[%d]: call=0x%08X ret=0x%08X arg0=0x%08X ptrA=0x%08X ptrB=0x%08X size=0x%08X result=0x%08X dispatch %02X/%02X -> %02X/%02X mode40 %02X->%02X seq71 %02X->%02X frame72 %02X->%02X frame73 %02X->%02X t76 %04X->%04X t78 %04X->%04X equip=%d slot=%d model 0x%08X->0x%08X pkan=0x%08X pseq=0x%08X",
            g_naturalAttackCommitLogBudget,
            callSite, returnAddr, arg0, ptrA, ptrB, size, result,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeMode40, *DISPATCHER_MODE40,
            beforeSeq71, *DISPATCHER_SEQ71,
            beforeFrame72, *DISPATCHER_FRAME72,
            beforeFrame73, *DISPATCHER_FRAME73,
            beforeTimer76, *DISPATCHER_TIMER76,
            beforeTimer78, *DISPATCHER_TIMER78,
            *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
            beforeModel, *MODEL_DIRECT_PTR,
            *PL_PKAN, *PL_PSEQ);
    }
    else if (naturalKnifeStable && g_naturalCommitLogBudget < 20)
    {
        g_naturalCommitLogBudget++;
        Log("Natural4010D0[%d]: call=0x%08X ret=0x%08X arg0=0x%08X ptrA=0x%08X ptrB=0x%08X size=0x%08X result=0x%08X dispatch %02X/%02X -> %02X/%02X mode40 %02X->%02X seq71 %02X->%02X frame72 %02X->%02X frame73 %02X->%02X t76 %04X->%04X t78 %04X->%04X equip=%d slot=%d model 0x%08X->0x%08X pkan=0x%08X pseq=0x%08X",
            g_naturalCommitLogBudget,
            callSite, returnAddr, arg0, ptrA, ptrB, size, result,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeMode40, *DISPATCHER_MODE40,
            beforeSeq71, *DISPATCHER_SEQ71,
            beforeFrame72, *DISPATCHER_FRAME72,
            beforeFrame73, *DISPATCHER_FRAME73,
            beforeTimer76, *DISPATCHER_TIMER76,
            beforeTimer78, *DISPATCHER_TIMER78,
            *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
            beforeModel, *MODEL_DIRECT_PTR,
            *PL_PKAN, *PL_PSEQ);
    }

    return result;
}

static void __cdecl PostCommitStageHook(u32 arg0)
{
    u32 returnAddr = (u32)_ReturnAddress();
    u32 callSite = (returnAddr >= 5u) ? (returnAddr - 5u) : 0u;
    u8 activeSlot = *PL_ACTIVE_SLOT;
    u8 activeSlotItem = 0;
    BOOL naturalKnifeEquipped = FALSE;
    if (activeSlot >= 1 && activeSlot <= 12)
        activeSlotItem = G_ITEM_WORK[(activeSlot - 1u) * 2u];
    naturalKnifeEquipped = (!g_knifeActive) &&
                           (*PL_EQUIP_ID == KNIFE_ID) &&
                           (activeSlotItem == KNIFE_ID) &&
                           (activeSlot != (u8)(KNIFE_SLOT + 1));

    u8 beforeState = *DISPATCHER_STATE;
    u8 beforeSub   = *DISPATCHER_SUBSTATE;
    u8 beforeMode40 = *DISPATCHER_MODE40;
    u8 beforeSeq71 = *DISPATCHER_SEQ71;
    u8 beforeFrame72 = *DISPATCHER_FRAME72;
    u8 beforeFrame73 = *DISPATCHER_FRAME73;
    u16 beforeTimer76 = *DISPATCHER_TIMER76;
    u16 beforeTimer78 = *DISPATCHER_TIMER78;
    u16 beforeKey = *G_KEY;
    u16 beforeTrg = *G_KEY_TRG;
    u32 beforeModel = *MODEL_DIRECT_PTR;
    BOOL knifeActiveNow = g_knifeActive ? TRUE : FALSE;
    BOOL invLatchedNow = g_inventoryLatched ? TRUE : FALSE;

    g_postStageTrampoline(arg0);

    u8 stateNow = *DISPATCHER_STATE;
    u8 subNow = *DISPATCHER_SUBSTATE;
    u32 modelNow = *MODEL_DIRECT_PTR;
    u32 tbl2Now = *(volatile u32*)WEAPON_MODEL_TABLE2;
    u16 keyNow = *G_KEY;
    BOOL tryLateNow = g_tryLateGameplayLwm ? TRUE : FALSE;
    BOOL lateDoneNow = g_lateGameplayLwmDone ? TRUE : FALSE;
    BOOL tryLateRestoreNow = g_tryLateRestoreLwm ? TRUE : FALSE;
    BOOL lateRestoreDoneNow = g_lateRestoreLwmDone ? TRUE : FALSE;
    u32 preReadyMask = 0;
    if (knifeActiveNow) preReadyMask |= 0x01u;
    if (!invLatchedNow) preReadyMask |= 0x02u;
    if (tryLateNow) preReadyMask |= 0x04u;
    if (!lateDoneNow) preReadyMask |= 0x08u;
    if (stateNow == 0x12) preReadyMask |= 0x10u;
    if (subNow == 1) preReadyMask |= 0x20u;
    if (tbl2Now == KNIFE_MODEL_PTR2 || tbl2Now == g_savedTbl2) preReadyMask |= 0x40u;
    if (modelNow == g_savedModelPtr1) preReadyMask |= 0x80u;
    BOOL preReadyCanFire = (preReadyMask == 0xFFu) &&
                           (callSite == 0x00409D18u) &&
                           ((keyNow & KEY_READY) != 0);
    u32 readyMask = 0;
    if (knifeActiveNow) readyMask |= 0x01u;
    if (!invLatchedNow) readyMask |= 0x02u;
    if (tryLateNow) readyMask |= 0x04u;
    if (!lateDoneNow) readyMask |= 0x08u;
    if (stateNow == 0x13) readyMask |= 0x10u;
    if (subNow == 1) readyMask |= 0x20u;
    if (tbl2Now == KNIFE_MODEL_PTR2 || tbl2Now == g_savedTbl2) readyMask |= 0x40u;
    if (modelNow == g_savedModelPtr1) readyMask |= 0x80u;
    BOOL readyCanFire = (readyMask == 0xFFu) &&
                        (callSite == 0x00409E8Cu) &&
                        ((keyNow & KEY_READY) != 0);
    u32 lateMask = 0;
    if (knifeActiveNow) lateMask |= 0x01u;
    if (!invLatchedNow) lateMask |= 0x02u;
    if (tryLateNow) lateMask |= 0x04u;
    if (!lateDoneNow) lateMask |= 0x08u;
    if (stateNow == 0x14) lateMask |= 0x10u;
    if (subNow == 1) lateMask |= 0x20u;
    if (tbl2Now == KNIFE_MODEL_PTR2 || tbl2Now == g_savedTbl2) lateMask |= 0x40u;
    if (modelNow == g_savedModelPtr1) lateMask |= 0x80u;
    BOOL lateCanFire = (lateMask == 0xFFu) ? TRUE : FALSE;

    if (knifeActiveNow &&
        !invLatchedNow &&
        g_lateLwmGateLogBudget < 16 &&
        (callSite == 0x0040A1DCu ||
         (beforeState == 0x14 && beforeSub == 1) ||
         (stateNow == 0x14 && subNow == 1)))
    {
        g_lateLwmGateLogBudget++;
        Log("LateGate[%d]: call=0x%08X dispatch %02X/%02X -> %02X/%02X try=%d done=%d inv=%d model=0x%08X savedModel=0x%08X modelEq=%d tbl2=0x%08X tbl2Eq=%d mask=0x%02X can=%d key=0x%04X",
            g_lateLwmGateLogBudget,
            callSite,
            beforeState, beforeSub, stateNow, subNow,
            tryLateNow ? 1 : 0,
            lateDoneNow ? 1 : 0,
            invLatchedNow ? 1 : 0,
            modelNow, g_savedModelPtr1,
            (modelNow == g_savedModelPtr1) ? 1 : 0,
            tbl2Now,
            (tbl2Now == KNIFE_MODEL_PTR2) ? 1 : 0,
            lateMask,
            lateCanFire ? 1 : 0,
            *G_KEY);
    }

    if (lateCanFire)
    {
        g_lateGameplayLwmDone = TRUE;
        Log("LateLWM: trigger state=%02X/%02X model=0x%08X tbl2=0x%08X",
            stateNow, subNow, modelNow, tbl2Now);
        LOAD_WEAPON_MODEL_FN(KNIFE_ID, WEAPON_MODEL_ARG1, WEAPON_MODEL_TABLE1, WEAPON_MODEL_TABLE2);
        *(volatile u32*)WEAPON_MODEL_TABLE2 = KNIFE_MODEL_PTR2;
        TrySavedRenderRefresh("LateLWM");
        Log("LateLWM: after model=0x%08X tbl2=0x%08X handler=0x%08X",
            *MODEL_DIRECT_PTR, *(volatile u32*)WEAPON_MODEL_TABLE2, *WEAPON_HANDLER_PTR);
    }

    if (knifeActiveNow &&
        !invLatchedNow &&
        g_preReadyLwmGateLogBudget < 10 &&
        (callSite == 0x00409D18u || preReadyCanFire))
    {
        g_preReadyLwmGateLogBudget++;
        Log("PreReadyGate[%d]: call=0x%08X dispatch %02X/%02X -> %02X/%02X try=%d done=%d model=0x%08X savedModel=0x%08X tbl2=0x%08X mask=0x%02X can=%d key=0x%04X",
            g_preReadyLwmGateLogBudget,
            callSite,
            beforeState, beforeSub, stateNow, subNow,
            tryLateNow ? 1 : 0,
            lateDoneNow ? 1 : 0,
            modelNow, g_savedModelPtr1,
            tbl2Now,
            preReadyMask,
            preReadyCanFire ? 1 : 0,
            keyNow);
    }

    if (preReadyCanFire)
    {
        g_lateGameplayLwmDone = TRUE;
        Log("PreReadyLWM: trigger state=%02X/%02X model=0x%08X tbl2=0x%08X",
            stateNow, subNow, modelNow, tbl2Now);
        LOAD_WEAPON_MODEL_FN(KNIFE_ID, WEAPON_MODEL_ARG1, WEAPON_MODEL_TABLE1, WEAPON_MODEL_TABLE2);
        *(volatile u32*)WEAPON_MODEL_TABLE2 = KNIFE_MODEL_PTR2;
        TrySavedRenderRefresh("PreReadyLWM");
        Log("PreReadyLWM: after model=0x%08X tbl2=0x%08X handler=0x%08X",
            *MODEL_DIRECT_PTR, *(volatile u32*)WEAPON_MODEL_TABLE2, *WEAPON_HANDLER_PTR);
    }

    if (knifeActiveNow &&
        !invLatchedNow &&
        g_readyLwmGateLogBudget < 12 &&
        (callSite == 0x00409E8Cu || readyCanFire))
    {
        g_readyLwmGateLogBudget++;
        Log("ReadyGate[%d]: call=0x%08X dispatch %02X/%02X -> %02X/%02X try=%d done=%d model=0x%08X savedModel=0x%08X tbl2=0x%08X mask=0x%02X can=%d key=0x%04X",
            g_readyLwmGateLogBudget,
            callSite,
            beforeState, beforeSub, stateNow, subNow,
            tryLateNow ? 1 : 0,
            lateDoneNow ? 1 : 0,
            modelNow, g_savedModelPtr1,
            tbl2Now,
            readyMask,
            readyCanFire ? 1 : 0,
            keyNow);
    }

    if (readyCanFire)
    {
        g_lateGameplayLwmDone = TRUE;
        Log("ReadyLWM: trigger state=%02X/%02X model=0x%08X tbl2=0x%08X",
            stateNow, subNow, modelNow, tbl2Now);
        LOAD_WEAPON_MODEL_FN(KNIFE_ID, WEAPON_MODEL_ARG1, WEAPON_MODEL_TABLE1, WEAPON_MODEL_TABLE2);
        *(volatile u32*)WEAPON_MODEL_TABLE2 = KNIFE_MODEL_PTR2;
        TrySavedRenderRefresh("ReadyLWM");
        Log("ReadyLWM: after model=0x%08X tbl2=0x%08X handler=0x%08X",
            *MODEL_DIRECT_PTR, *(volatile u32*)WEAPON_MODEL_TABLE2, *WEAPON_HANDLER_PTR);
    }

    {
        u32 restoreMask = 0;
        if (!knifeActiveNow) restoreMask |= 0x01u;
        if (!invLatchedNow) restoreMask |= 0x02u;
        if (tryLateRestoreNow) restoreMask |= 0x04u;
        if (!lateRestoreDoneNow) restoreMask |= 0x08u;
        if (*PL_EQUIP_ID == g_savedEquipId) restoreMask |= 0x10u;
        if (*PL_ACTIVE_SLOT == g_savedSlot) restoreMask |= 0x20u;
        /* Restore already writes g_savedTbl2 immediately before LWM, so waiting for tbl2
           to naturally flip back from knife-side only stretches the dead window between
           RB release and the next valid RB press. Accept either the saved pistol tbl2 or
           the current knife tbl2 here and let LateRestoreLWM perform the real swap. */
        if (tbl2Now == g_savedTbl2 || tbl2Now == KNIFE_MODEL_PTR2) restoreMask |= 0x40u;
        if (modelNow != 0 && modelNow != g_savedModelPtr1) restoreMask |= 0x80u;
        BOOL lateRestoreCanFire = (restoreMask == 0xFFu) ? TRUE : FALSE;

        if (!knifeActiveNow &&
            !invLatchedNow &&
            g_postRestoreFrames > 0 &&
            g_lateRestoreGateLogBudget < 16 &&
            (tryLateRestoreNow ||
             callSite == 0x0040A1DCu ||
             modelNow != g_savedModelPtr1))
        {
            g_lateRestoreGateLogBudget++;
            Log("LateRestoreGate[%d]: call=0x%08X dispatch %02X/%02X -> %02X/%02X try=%d done=%d frames=%d model=0x%08X savedModel=0x%08X tbl2=0x%08X savedTbl2=0x%08X mask=0x%02X can=%d equip=%d slot=%d key=0x%04X",
                g_lateRestoreGateLogBudget,
                callSite,
                beforeState, beforeSub, stateNow, subNow,
                tryLateRestoreNow ? 1 : 0,
                lateRestoreDoneNow ? 1 : 0,
                g_postRestoreFrames,
                modelNow, g_savedModelPtr1,
                tbl2Now, g_savedTbl2,
                restoreMask,
                lateRestoreCanFire ? 1 : 0,
                *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
                *G_KEY);
        }

        if (lateRestoreCanFire)
        {
            g_tryLateRestoreLwm = FALSE;
            *PL_EQUIP_ID = g_savedEquipId;
            *PL_ACTIVE_SLOT = g_savedSlot;
            *WEAPON_HANDLER_PTR = g_savedHandlerPtr;
            *(volatile u32*)WEAPON_MODEL_TABLE2 = g_savedTbl2;
            Log("LateRestoreLWM: trigger equip=%d item=%d slot=%d model=0x%08X tbl2=0x%08X",
                *PL_EQUIP_ID, g_savedItemId, *PL_ACTIVE_SLOT, *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2);
            LOAD_WEAPON_MODEL_FN((u32)g_savedItemId, WEAPON_MODEL_ARG1, WEAPON_MODEL_TABLE1, WEAPON_MODEL_TABLE2);
            /* Clear dirty flag after forced LWM so the game's SyncEquip pipeline
               doesn't immediately re-trigger another equip dispatch cycle, which
               causes the aiming animation loop when LB is held after restore. */
            *WEAPON_DIRTY_FLAG = 0x00;
            g_lateRestoreLwmDone = TRUE;
            Log("LateRestoreLWM: after model=0x%08X tbl2=0x%08X handler=0x%08X dirty=0x%02X",
                *MODEL_DIRECT_PTR, *(volatile u32*)WEAPON_MODEL_TABLE2, *WEAPON_HANDLER_PTR,
                *WEAPON_DIRTY_FLAG);
        }
    }

    if (g_knifeActive && g_activePostStageLogBudget < 40)
    {
        g_activePostStageLogBudget++;
        Log("ActiveA670[%d]: call=0x%08X ret=0x%08X arg0=0x%08X key %04X->%04X trg %04X->%04X dispatch %02X/%02X -> %02X/%02X mode40 %02X->%02X seq71 %02X->%02X frame72 %02X->%02X frame73 %02X->%02X t76 %04X->%04X t78 %04X->%04X equip=%d slot=%d model 0x%08X->0x%08X pkan=0x%08X pseq=0x%08X",
            g_activePostStageLogBudget,
            callSite, returnAddr, arg0,
            beforeKey, *G_KEY,
            beforeTrg, *G_KEY_TRG,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeMode40, *DISPATCHER_MODE40,
            beforeSeq71, *DISPATCHER_SEQ71,
            beforeFrame72, *DISPATCHER_FRAME72,
            beforeFrame73, *DISPATCHER_FRAME73,
            beforeTimer76, *DISPATCHER_TIMER76,
            beforeTimer78, *DISPATCHER_TIMER78,
            *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
            beforeModel, *MODEL_DIRECT_PTR,
            *PL_PKAN, *PL_PSEQ);
    }
    else if (naturalKnifeEquipped && g_naturalPostStageLogBudget < 20)
    {
        g_naturalPostStageLogBudget++;
        Log("NaturalA670[%d]: call=0x%08X ret=0x%08X arg0=0x%08X key %04X->%04X trg %04X->%04X dispatch %02X/%02X -> %02X/%02X mode40 %02X->%02X seq71 %02X->%02X frame72 %02X->%02X frame73 %02X->%02X t76 %04X->%04X t78 %04X->%04X equip=%d slot=%d model 0x%08X->0x%08X pkan=0x%08X pseq=0x%08X",
            g_naturalPostStageLogBudget,
            callSite, returnAddr, arg0,
            beforeKey, *G_KEY,
            beforeTrg, *G_KEY_TRG,
            beforeState, beforeSub, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
            beforeMode40, *DISPATCHER_MODE40,
            beforeSeq71, *DISPATCHER_SEQ71,
            beforeFrame72, *DISPATCHER_FRAME72,
            beforeFrame73, *DISPATCHER_FRAME73,
            beforeTimer76, *DISPATCHER_TIMER76,
            beforeTimer78, *DISPATCHER_TIMER78,
            *PL_EQUIP_ID, *PL_ACTIVE_SLOT,
            beforeModel, *MODEL_DIRECT_PTR,
            *PL_PKAN, *PL_PSEQ);
    }
}

static void InstallCommitHook(void)
{
    u8* site = COMMIT_HOOK_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
        { Log("CommitHook: VirtualProtect failed"); return; }
    if (site[0] == 0xE9)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("CommitHook: already patched"); return; }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("CommitHook: patchSize=0, bytes=0x%02X 0x%02X 0x%02X", site[0], site[1], site[2]); return; }

    g_commitStub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_commitStub)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("CommitHook: VirtualAlloc failed"); return; }

    memcpy(g_commitOrigBytes, site, patchSize);
    g_commitPatchSize = patchSize;

    u8* p = g_commitStub;
    memcpy(p, site, patchSize); p += patchSize;
    *p++ = 0xE9;
    *(s32*)p = (s32)((site + patchSize) - (p + 4)); p += 4;
    g_commitTrampoline = (CommitAnimFn)g_commitStub;

    site[0] = 0xE9;
    *(s32*)(site + 1) = (s32)((u8*)CommitAnimHook - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_commitHooked = TRUE;
    Log("CommitHook: installed at 0x004010D0, patchSize=%d, stub=0x%08X", patchSize, (unsigned)g_commitStub);
}

static void UninstallCommitHook(void)
{
    if (!g_commitHooked || !g_commitStub) return;
    u8* site = COMMIT_HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_commitOrigBytes, g_commitPatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);
    VirtualFree(g_commitStub, 0, MEM_RELEASE);
    g_commitStub = NULL;
    g_commitHooked = FALSE;
    Log("CommitHook: uninstalled");
}

static void InstallPostStageHook(void)
{
    u8* site = POST_STAGE_HOOK_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
        { Log("PostStageHook: VirtualProtect failed"); return; }
    if (site[0] == 0xE9)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("PostStageHook: already patched"); return; }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("PostStageHook: patchSize=0, bytes=0x%02X 0x%02X 0x%02X", site[0], site[1], site[2]); return; }

    g_postStageStub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_postStageStub)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("PostStageHook: VirtualAlloc failed"); return; }

    memcpy(g_postStageOrigBytes, site, patchSize);
    g_postStagePatchSize = patchSize;

    u8* p = g_postStageStub;
    memcpy(p, site, patchSize); p += patchSize;
    *p++ = 0xE9;
    *(s32*)p = (s32)((site + patchSize) - (p + 4)); p += 4;
    g_postStageTrampoline = (PostCommitStageFn)g_postStageStub;

    site[0] = 0xE9;
    *(s32*)(site + 1) = (s32)((u8*)PostCommitStageHook - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_postStageHooked = TRUE;
    Log("PostStageHook: installed at 0x0040A670, patchSize=%d, stub=0x%08X", patchSize, (unsigned)g_postStageStub);
}

static void UninstallPostStageHook(void)
{
    if (!g_postStageHooked || !g_postStageStub) return;
    u8* site = POST_STAGE_HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_postStageOrigBytes, g_postStagePatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);
    VirtualFree(g_postStageStub, 0, MEM_RELEASE);
    g_postStageStub = NULL;
    g_postStageHooked = FALSE;
    Log("PostStageHook: uninstalled");
}

/* ================================================================
   FLIP HOOK (0x4343A0 — present/flip, fires after render each frame)
   Writes the deferred dispatch redirect AFTER the frame has been
   rendered, so the ENGAGE frame renders the previous weapon normally
   instead of showing an invisible flash.
   ================================================================ */
static void __cdecl FlipHook(void)
{
    if (g_pendingDispatchRedirect)
    {
        g_pendingDispatchRedirect = FALSE;
        *(volatile u32*)0xC35238U = 0x02000001U;
        *WEAPON_DIRTY_FLAG = 0xFF;
        Log("FlipHook: deferred dispatch redirect written dispatch=%02X/%02X",
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
    }
    g_flipTrampoline();
}

static void InstallFlipHook(void)
{
    u8* site = FLIP_HOOK_SITE;
    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
        { Log("FlipHook: VirtualProtect failed"); return; }
    if (site[0] == 0xE9)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("FlipHook: already patched"); return; }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("FlipHook: patchSize=0, bytes=0x%02X 0x%02X 0x%02X", site[0], site[1], site[2]); return; }

    g_flipStub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_flipStub)
        { VirtualProtect(site, 16, oldProt, &oldProt); Log("FlipHook: VirtualAlloc failed"); return; }

    memcpy(g_flipOrigBytes, site, patchSize);
    g_flipPatchSize = patchSize;

    u8* p = g_flipStub;
    memcpy(p, site, patchSize); p += patchSize;
    *p++ = 0xE9;
    *(s32*)p = (s32)((site + patchSize) - (p + 4)); p += 4;
    g_flipTrampoline = (FlipFn)g_flipStub;

    site[0] = 0xE9;
    *(s32*)(site + 1) = (s32)((u8*)FlipHook - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_flipHooked = TRUE;
    Log("FlipHook: installed at 0x4343A0, patchSize=%d, stub=0x%08X", patchSize, (unsigned)g_flipStub);
}

static void UninstallFlipHook(void)
{
    if (!g_flipHooked || !g_flipStub) return;
    u8* site = FLIP_HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_flipOrigBytes, g_flipPatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);
    VirtualFree(g_flipStub, 0, MEM_RELEASE);
    g_flipStub = NULL;
    g_flipHooked = FALSE;
    Log("FlipHook: uninstalled");
}

/* ================================================================
   DIRECTDRAW SURFACE FLIP HOOK
   Hooks IDirectDrawSurface::Flip via COM vtable patching (slot 11).
   Strategy: create a temporary 1x1 surface at mod init to get the
   vtable pointer; patch slot 11 in-place. All IDirectDrawSurface
   instances of the same COM class share one vtable, so this intercepts
   the game's primary surface Flip without needing its address.

   When g_suppressNextFlip is set (written by the equip one-shot
   alongside the dispatch redirect), we copy the current front buffer
   into the back buffer BEFORE flipping. The Flip then presents a copy
   of the previous frame, hiding the dispatch-redirect invisible frame.
   ================================================================ */
static HRESULT __stdcall DDSFlipHook(IDirectDrawSurface* pThis,
                                     IDirectDrawSurface* pDDSOverride,
                                     DWORD dwFlags)
{
    BOOL suppressThisFlip = FALSE;
    u32 modelNow = *MODEL_DIRECT_PTR;
    u32 tbl2Now = *(volatile u32*)WEAPON_MODEL_TABLE2;
    BOOL gameplayKnifeActive = g_knifeActive && !g_inventoryLatched;
    BOOL knifeVisualReady = gameplayKnifeActive && IsGameplayKnifeVisualReady();

    if (g_suppressNextFlip || g_entryFlipHoldBudget > 0 || g_entryFlipSettleBudget > 0)
    {
        if (g_suppressNextFlip)
        {
            g_suppressNextFlip = FALSE;
            suppressThisFlip = TRUE;
        }

        if (g_entryFlipHoldBudget > 0)
        {
            if (!knifeVisualReady)
            {
                suppressThisFlip = TRUE;
                g_entryFlipHoldBudget--;

                if (g_entryFlipLogBudget < 12)
                {
                    g_entryFlipLogBudget++;
                    Log("DDSFlipHold[%d]: waiting model=0x%08X saved=0x%08X tbl2=0x%08X dispatch=%02X/%02X budget=%d",
                        g_entryFlipLogBudget,
                        modelNow, g_savedModelPtr1, tbl2Now,
                        *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                        g_entryFlipHoldBudget);
                }
            }
            else
            {
                if (g_entryFlipSettleBudget == 0)
                {
                    g_entryFlipSettleBudget = 2;
                    if (g_entryFlipLogBudget < 12)
                    {
                        g_entryFlipLogBudget++;
                        Log("DDSFlipHold[%d]: knife visual ready model=0x%08X tbl2=0x%08X dispatch=%02X/%02X",
                            g_entryFlipLogBudget,
                            modelNow, tbl2Now,
                            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
                    }
                }

                if (g_entryFlipSettleBudget > 0)
                {
                    suppressThisFlip = TRUE;
                    g_entryFlipSettleBudget--;
                }

                if (g_entryFlipSettleBudget == 0)
                {
                    g_entryFlipHoldBudget = 0;
                }
            }
        }
        else if (g_entryFlipSettleBudget > 0)
        {
            suppressThisFlip = TRUE;
            g_entryFlipSettleBudget--;
        }

        if (!gameplayKnifeActive)
        {
            g_entryFlipHoldBudget = 0;
            g_entryFlipSettleBudget = 0;
        }

        /* Copy front (pThis) -> back buffer, overwriting the rendered flicker frame.
           After Flip, screen shows the copy of the previous frame — no visual change. */
        if (suppressThisFlip)
        {
        DDSCAPS caps;
        ZeroMemory(&caps, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        IDirectDrawSurface* pBack = NULL;
        HRESULT hr2 = pThis->GetAttachedSurface(&caps, &pBack);
        if (SUCCEEDED(hr2) && pBack)
        {
            HRESULT hr3 = pBack->Blt(NULL, pThis, NULL, DDBLT_WAIT, NULL);
            pBack->Release();
            Log("DDSFlipHook: flicker frame suppressed (front->back blt hr=0x%08X)", (u32)hr3);
        }
        else
        {
            Log("DDSFlipHook: suppress requested but GetAttachedSurface failed hr=0x%08X (flip proceeds normally)", (u32)hr2);
        }
        }
    }
    return g_origDDSFlip(pThis, pDDSOverride, dwFlags);
}

static HRESULT __stdcall DDPresentBltHook(IDirectDrawSurface* pThis,
                                          LPRECT lpDestRect,
                                          IDirectDrawSurface* pSrcSurface,
                                          LPRECT lpSrcRect,
                                          DWORD dwFlags,
                                          LPDDBLTFX lpDDBltFx);
static HRESULT __stdcall DDPresentBltFastHook(IDirectDrawSurface* pThis,
                                              DWORD dwX,
                                              DWORD dwY,
                                              IDirectDrawSurface* pSrcSurface,
                                              LPRECT lpSrcRect,
                                              DWORD dwTrans);
static HRESULT __stdcall DDPresentFlipHook(IDirectDrawSurface* pThis,
                                           IDirectDrawSurface* pDDSOverride,
                                           DWORD dwFlags);
static HRESULT __stdcall DDPresentCreateSurfaceHook(IDirectDraw* pThis,
                                                    DDSURFACEDESC* pDesc,
                                                    IDirectDrawSurface** ppSurf,
                                                    IUnknown* pOuter);

static BOOL TryPatchPresentSurfaceVtable(IDirectDrawSurface* pSurf, const char* tag)
{
    if (!pSurf)
        return FALSE;

    if (LookupDDSurfaceHook(pSurf))
        return TRUE;

    int slot = -1;
    for (int i = 0; i < DD_SURFACE_HOOK_MAX; ++i)
    {
        if (!g_ddSurfaceHooks[i].active)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        Log("DDPresentHook: no free vtable slot for %s", tag);
        return FALSE;
    }

    void** vtable = *(void***)pSurf;
    DWORD caps = 0;
    TryGetDDSurfaceCaps(pSurf, &caps);

    DWORD oldProt;
    if (!VirtualProtect(&vtable[5], sizeof(void*) * 7, PAGE_READWRITE, &oldProt))
    {
        Log("DDPresentHook: VirtualProtect failed for %s vtable=0x%08X",
            tag, (u32)(DWORD_PTR)vtable);
        return FALSE;
    }

    g_ddSurfaceHooks[slot].vtable      = vtable;
    g_ddSurfaceHooks[slot].origBlt     = (DDSBltFn)vtable[5];
    g_ddSurfaceHooks[slot].origBltFast = (DDSBltFastFn)vtable[7];
    g_ddSurfaceHooks[slot].origFlip    = (DDSFlipFn)vtable[11];
    g_ddSurfaceHooks[slot].active      = TRUE;

    vtable[5]  = (void*)DDPresentBltHook;
    vtable[7]  = (void*)DDPresentBltFastHook;
    vtable[11] = (void*)DDPresentFlipHook;

    VirtualProtect(&vtable[5], sizeof(void*) * 7, oldProt, &oldProt);
    Log("DDPresentHook: patched %s vtable=0x%08X caps=0x%08X blt=0x%08X bltFast=0x%08X flip=0x%08X",
        tag,
        (u32)(DWORD_PTR)vtable,
        caps,
        (u32)(DWORD_PTR)g_ddSurfaceHooks[slot].origBlt,
        (u32)(DWORD_PTR)g_ddSurfaceHooks[slot].origBltFast,
        (u32)(DWORD_PTR)g_ddSurfaceHooks[slot].origFlip);
    return TRUE;
}

static void ProbePresentCandidateSurface(IDirectDraw* pDD,
                                         const char* tag,
                                         DWORD caps,
                                         DWORD extraFlags,
                                         DWORD width,
                                         DWORD height,
                                         DWORD backBufferCount)
{
    DDSURFACEDESC ddsd;
    ZeroMemory(&ddsd, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | extraFlags;
    ddsd.ddsCaps.dwCaps = caps;
    ddsd.dwWidth = width;
    ddsd.dwHeight = height;
    ddsd.dwBackBufferCount = backBufferCount;

    IDirectDrawSurface* pSurf = NULL;
    HRESULT hr = pDD->CreateSurface(&ddsd, &pSurf, NULL);
    if (FAILED(hr) || !pSurf)
    {
        Log("DDPresentHook: probe %s CreateSurface failed hr=0x%08X caps=0x%08X",
            tag, (u32)hr, caps);
        return;
    }

    TryPatchPresentSurfaceVtable(pSurf, tag);
    pSurf->Release();
}

static HRESULT __stdcall DDPresentBltHook(IDirectDrawSurface* pThis,
                                          LPRECT lpDestRect,
                                          IDirectDrawSurface* pSrcSurface,
                                          LPRECT lpSrcRect,
                                          DWORD dwFlags,
                                          LPDDBLTFX lpDDBltFx)
{
    DDSurfaceHookEntry* entry = LookupDDSurfaceHook(pThis);
    if (!entry || !entry->origBlt)
        return DDERR_GENERIC;

    if (ShouldSuppressPresentNow("Blt", pThis))
        return DD_OK;

    return entry->origBlt(pThis, lpDestRect, pSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
}

static HRESULT __stdcall DDPresentBltFastHook(IDirectDrawSurface* pThis,
                                              DWORD dwX,
                                              DWORD dwY,
                                              IDirectDrawSurface* pSrcSurface,
                                              LPRECT lpSrcRect,
                                              DWORD dwTrans)
{
    DDSurfaceHookEntry* entry = LookupDDSurfaceHook(pThis);
    if (!entry || !entry->origBltFast)
        return DDERR_GENERIC;

    if (ShouldSuppressPresentNow("BltFast", pThis))
        return DD_OK;

    return entry->origBltFast(pThis, dwX, dwY, pSrcSurface, lpSrcRect, dwTrans);
}

static HRESULT __stdcall DDPresentFlipHook(IDirectDrawSurface* pThis,
                                           IDirectDrawSurface* pDDSOverride,
                                           DWORD dwFlags)
{
    DDSurfaceHookEntry* entry = LookupDDSurfaceHook(pThis);
    if (!entry || !entry->origFlip)
        return DDERR_GENERIC;

    if (ShouldSuppressPresentNow("Flip", pThis))
        return DD_OK;

    return entry->origFlip(pThis, pDDSOverride, dwFlags);
}

static HRESULT __stdcall DDPresentCreateSurfaceHook(IDirectDraw* pThis,
                                                    DDSURFACEDESC* pDesc,
                                                    IDirectDrawSurface** ppSurf,
                                                    IUnknown* pOuter)
{
    if (!g_origDDCreateSurface)
        return DDERR_GENERIC;

    HRESULT hr = g_origDDCreateSurface(pThis, pDesc, ppSurf, pOuter);
    if (g_ddCreateLogBudget < 16)
    {
        g_ddCreateLogBudget++;
        Log("DDCreateSurface[%d]: hr=0x%08X flags=0x%08X caps=0x%08X backBuffers=%u surf=0x%08X",
            g_ddCreateLogBudget,
            (u32)hr,
            pDesc ? pDesc->dwFlags : 0,
            pDesc ? pDesc->ddsCaps.dwCaps : 0,
            pDesc ? pDesc->dwBackBufferCount : 0,
            (u32)(DWORD_PTR)((ppSurf && *ppSurf) ? *ppSurf : NULL));
    }

    if (SUCCEEDED(hr) && ppSurf && *ppSurf)
        TryPatchPresentSurfaceVtable(*ppSurf, "CreateSurface");

    return hr;
}

typedef HRESULT (WINAPI* DDCreateFn)(GUID*, IDirectDraw**, IUnknown*);

static void InstallDDFlipHook(void)
{
    HMODULE hDDraw = GetModuleHandle("ddraw.dll");
    if (!hDDraw) { Log("DDPresentHook: ddraw.dll not loaded"); return; }

    DDCreateFn pfnCreate = (DDCreateFn)GetProcAddress(hDDraw, "DirectDrawCreate");
    if (!pfnCreate) { Log("DDPresentHook: DirectDrawCreate not found in ddraw.dll"); return; }

    IDirectDraw* pDD = NULL;
    HRESULT hr = pfnCreate(NULL, &pDD, NULL);
    if (FAILED(hr) || !pDD)
    {
        Log("DDPresentHook: DirectDrawCreate failed hr=0x%08X", (u32)hr);
        return;
    }

    HWND hCoopWnd = GetForegroundWindow();
    if (!hCoopWnd)
        hCoopWnd = GetDesktopWindow();

    HRESULT hrCoop = pDD->SetCooperativeLevel(hCoopWnd, DDSCL_NORMAL);
    if (FAILED(hrCoop))
    {
        Log("DDPresentHook: SetCooperativeLevel failed hr=0x%08X hwnd=0x%08X",
            (u32)hrCoop, (u32)(DWORD_PTR)hCoopWnd);
    }

    void** ddVtable = *(void***)pDD;
    if (!g_origDDCreateSurface && ddVtable)
    {
        DWORD oldProt;
        if (VirtualProtect(&ddVtable[6], sizeof(void*), PAGE_READWRITE, &oldProt))
        {
            g_ddCreateVtable = ddVtable;
            g_origDDCreateSurface = (DDCreateSurfaceMethodFn)ddVtable[6];
            ddVtable[6] = (void*)DDPresentCreateSurfaceHook;
            VirtualProtect(&ddVtable[6], sizeof(void*), oldProt, &oldProt);
            Log("DDPresentHook: CreateSurface patched orig=0x%08X hook=0x%08X",
                (u32)(DWORD_PTR)g_origDDCreateSurface,
                (u32)(DWORD_PTR)DDPresentCreateSurfaceHook);
        }
        else
        {
            Log("DDPresentHook: VirtualProtect failed for CreateSurface slot");
        }
    }

    ProbePresentCandidateSurface(pDD, "offscreen-1x1", DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY,
        DDSD_WIDTH | DDSD_HEIGHT, 1, 1, 0);
    ProbePresentCandidateSurface(pDD, "primary", DDSCAPS_PRIMARYSURFACE,
        0, 0, 0, 0);
    ProbePresentCandidateSurface(pDD, "primary-flip-chain", DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX,
        DDSD_BACKBUFFERCOUNT, 0, 0, 1);

    IDirectDrawSurface* pGDISurface = NULL;
    hr = pDD->GetGDISurface(&pGDISurface);
    if (SUCCEEDED(hr) && pGDISurface)
    {
        TryPatchPresentSurfaceVtable(pGDISurface, "gdi-surface");
        pGDISurface->Release();
    }
    else
    {
        Log("DDPresentHook: GetGDISurface failed hr=0x%08X", (u32)hr);
    }

    pDD->Release();
}

static void UninstallDDFlipHook(void)
{
    for (int i = 0; i < DD_SURFACE_HOOK_MAX; ++i)
    {
        if (!g_ddSurfaceHooks[i].active || !g_ddSurfaceHooks[i].vtable)
            continue;

        DWORD oldProt;
        if (VirtualProtect(&g_ddSurfaceHooks[i].vtable[5], sizeof(void*) * 7, PAGE_READWRITE, &oldProt))
        {
            g_ddSurfaceHooks[i].vtable[5]  = (void*)g_ddSurfaceHooks[i].origBlt;
            g_ddSurfaceHooks[i].vtable[7]  = (void*)g_ddSurfaceHooks[i].origBltFast;
            g_ddSurfaceHooks[i].vtable[11] = (void*)g_ddSurfaceHooks[i].origFlip;
            VirtualProtect(&g_ddSurfaceHooks[i].vtable[5], sizeof(void*) * 7, oldProt, &oldProt);
            Log("DDPresentHook: restored vtable=0x%08X", (u32)(DWORD_PTR)g_ddSurfaceHooks[i].vtable);
        }

        g_ddSurfaceHooks[i].vtable = NULL;
        g_ddSurfaceHooks[i].origBlt = NULL;
        g_ddSurfaceHooks[i].origBltFast = NULL;
        g_ddSurfaceHooks[i].origFlip = NULL;
        g_ddSurfaceHooks[i].active = FALSE;
    }

    if (g_origDDCreateSurface && g_ddCreateVtable)
    {
        DWORD oldProt;
        if (VirtualProtect(&g_ddCreateVtable[6], sizeof(void*), PAGE_READWRITE, &oldProt))
        {
            g_ddCreateVtable[6] = (void*)g_origDDCreateSurface;
            VirtualProtect(&g_ddCreateVtable[6], sizeof(void*), oldProt, &oldProt);
            Log("DDPresentHook: CreateSurface restored");
        }
    }

    g_origDDCreateSurface = NULL;
    g_ddCreateVtable = NULL;
}

/* ================================================================
   HOOK INJECT FUNCTION
   Called from our stub in the game's main thread, after the game
   has written G_KEY and G_KEY_TRG for this frame.
   Only safe for direct memory writes — no game function calls here.
   ================================================================ */
static BOOL s_prevKnifeActive = FALSE;
static BOOL s_needKnifeReadyRetrigger = FALSE;
static int  s_knifeHeldDiagFrame = 0;
static int  s_naturalKnifeStableFrames = 0;
static int  s_naturalKnifeDiagFrame = 0;
static int  s_readyRetriggerLogBudget = 0;

static void __cdecl QuickKnifeInjectFn(void)
{
    if (!g_knifeActive)
    {
        u8 activeSlot = *PL_ACTIVE_SLOT;
        u8 activeSlotItem = 0;
        if (activeSlot >= 1 && activeSlot <= 12)
            activeSlotItem = G_ITEM_WORK[(activeSlot - 1u) * 2u];

        /* Filter out the brief post-RB restore window where PL_EQUIP_ID can still be
           knife for a frame or two even though the real active inventory slot is
           already back on pistol. We only want stable, natural knife snapshots. */
        BOOL naturalKnifeStable = (*PL_EQUIP_ID == KNIFE_ID) &&
                                  (activeSlotItem == KNIFE_ID) &&
                                  (activeSlot != (u8)(KNIFE_SLOT + 1));

        if (naturalKnifeStable)
        {
            s_naturalKnifeStableFrames++;
            if (s_naturalKnifeStableFrames >= 3 && s_naturalKnifeDiagFrame < 20)
            {
                s_naturalKnifeDiagFrame++;

                u32 psin14Ptr = 0;
                u32 psin14_0C = 0;
                u32 psin14_18 = 0;
                u32 pSin = *PSIN_PARTS_PTR;
                if (pSin > 0x400000u)
                {
                    psin14_0C = *(volatile u32*)(pSin + 14u * 124u + 0x0Cu);
                    psin14Ptr = *(volatile u32*)(pSin + 14u * 124u + 0x14u);
                    psin14_18 = *(volatile u32*)(pSin + 14u * 124u + 0x18u);
                }

                int firstDiff = -1;
                if (g_hasKnifeAnimBuffer)
                {
                    for (u32 i = 0; i < WEAPON_ANIM_BUFFER_SIZE; ++i)
                    {
                        if (((const u8*)WEAPON_MODEL_TABLE1)[i] != g_cachedKnifeAnimBuffer[i])
                        {
                            firstDiff = (int)i;
                            break;
                        }
                    }
                }

                Log("NaturalKnifeStable[%d]: slot=%d slotItem=%d equip=%d handler=0x%08X model=0x%08X tbl2=0x%08X pkan=0x%08X pseq=0x%08X dispatch=%02X/%02X",
                    s_naturalKnifeDiagFrame, *PL_ACTIVE_SLOT, activeSlotItem, *PL_EQUIP_ID,
                    *WEAPON_HANDLER_PTR, *MODEL_DIRECT_PTR,
                    *(volatile u32*)WEAPON_MODEL_TABLE2, *PL_PKAN, *PL_PSEQ,
                    *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
                Log("NaturalKnifeStable[%d]: psin14+0C=0x%08X psin14+14=0x%08X psin14+18=0x%08X preloadKnifeModel=0x%08X",
                    s_naturalKnifeDiagFrame,
                    psin14_0C, psin14Ptr, psin14_18, g_knifeModelDynamic);
                Log("NaturalKnifeStable[%d]: animBuffer preloadMatch=%d firstDiff=%d",
                    s_naturalKnifeDiagFrame,
                    (g_hasKnifeAnimBuffer && firstDiff < 0) ? 1 : 0, firstDiff);
            }
        }
        else
        {
            s_naturalKnifeStableFrames = 0;
            s_naturalKnifeDiagFrame = 0;
            g_naturalCommitLogBudget = 0;
            g_naturalPostStageLogBudget = 0;
        }
    }

    /* One-shot equip: save state, set up knife audio, force model+handler pointers */
    if (g_needEquipLoad)
    {
        g_needEquipLoad = FALSE;
        s_knifeHeldDiagFrame = 0;
        g_ddPresentLogBudget = 0;
        g_syncEquipLogBudget = 0;
        g_activeLwmLogBudget = 0;
        g_activeCommitLogBudget = 0;
        g_activePostStageLogBudget = 0;
        g_preReadyLwmGateLogBudget = 0;
        g_readyLwmGateLogBudget = 0;
        g_lateLwmGateLogBudget = 0;
        g_lateRestoreGateLogBudget = 0;
        g_disp0LogBudget = 0;
        g_lateGameplayLwmDone = FALSE;
        g_tryLateRestoreLwm = FALSE;
        g_lateRestoreLwmDone = FALSE;

        /* Save pistol weapon state blob for restore */
        memcpy(g_pistolWState, (const void*)WSTATE_BASE, WSTATE_SIZE);

        /* === BUILD 12: DISPATCH REDIRECT ===
           Force STATE=0, SUBSTATE=2 so the game's own FUN_00429040 case 2
           fires on the next dispatch cycle. Case 2 calls LWM(PL_EQUIP_ID)
           from the game's legal stack frame — the only known safe way to
           get a full model load.
           1. Set PL_EQUIP_ID = knife (so case 2 loads knife model)
           2. Call LOAD_WEAPON_FN for audio/table setup (safe from hook)
           3. Keep old secondary render table until legal knife LWM lands
           4. Write DWORD[C35238] = STATE=0, SUBSTATE=2
           5. Set dirty flag */
        *PL_EQUIP_ID = KNIFE_ID;
        LOAD_WEAPON_FN(KNIFE_ID, WEAPON_TABLE_PTR);
        *(volatile u32*)0xC35238U = 0x02000001U;  /* byte0=1, STATE=0, SUBSTATE=2 */
        *WEAPON_DIRTY_FLAG = 0xFF;
        g_suppressNextFlip = TRUE;  /* hide first redirect frame immediately */
        g_entryFlipHoldBudget = 8;  /* keep showing the last good frame until knife model is really live */
        g_entryFlipSettleBudget = 0;
        g_entryFlipLogBudget = 0;
        g_restoreFlipHoldBudget = 0;
        g_restoreFlipSettleBudget = 0;
        g_restoreFlipLogBudget = 0;
        g_tryLateGameplayLwm = TRUE;
        Log("RB cycle[%d]: equip armed first=%d hold=%d settle=%d savedModel=0x%08X savedTbl2=0x%08X savedHandler=0x%08X",
            g_activeRbCycle,
            g_suppressNextFlip ? 1 : 0,
            g_entryFlipHoldBudget,
            g_entryFlipSettleBudget,
            g_savedModelPtr1,
            g_savedTbl2,
            g_savedHandlerPtr);

        Log("equip: dispatch redirect — slot=%d equip=%d dispatch=%02X/%02X model=0x%08X handler=0x%08X pkan=0x%08X pseq=0x%08X",
            *PL_ACTIVE_SLOT, *PL_EQUIP_ID,
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, *MODEL_DIRECT_PTR, *WEAPON_HANDLER_PTR,
            *PL_PKAN, *PL_PSEQ);
    }

    if (g_needRestoreLoad)
    {
        g_needRestoreLoad = FALSE;
        g_ddPresentLogBudget = 0;
        g_tryLateGameplayLwm = FALSE;
        g_lateRestoreGateLogBudget = 0;

        /* === BUILD 12: DISPATCH REDIRECT FOR RESTORE ===
           Same approach as equip: set PL_EQUIP_ID to saved weapon,
           call LOAD_WEAPON_FN, restore handler, trigger case 2 → LWM(pistol). */
        *PL_EQUIP_ID = g_savedEquipId;
        LOAD_WEAPON_FN((u32)g_savedItemId, WEAPON_TABLE_PTR);
        *WEAPON_HANDLER_PTR = g_savedHandlerPtr;
        *(volatile u32*)WEAPON_MODEL_TABLE2 = g_savedTbl2;
        *WEAPON_DIRTY_FLAG = 0xFF;
        /* If swingDone already armed the budget (drain path), don't reduce it.
           Otherwise arm it fresh for normal (non-swing) RB releases. */
        if (g_restoreFlipHoldBudget < 8)
            g_restoreFlipHoldBudget = 8;
        g_restoreFlipSettleBudget = 0;
        g_restoreFlipLogBudget = 0;
        g_postRestoreFrames = 60;
        Log("RB cycle[%d]: restore armed first=%d hold=%d settle=%d model=0x%08X tbl2=0x%08X handler=0x%08X post=%d",
            g_completedRbCycle,
            g_suppressNextFlip ? 1 : 0,
            g_entryFlipHoldBudget,
            g_entryFlipSettleBudget,
            *MODEL_DIRECT_PTR,
            *(volatile u32*)WEAPON_MODEL_TABLE2,
            *WEAPON_HANDLER_PTR,
            g_postRestoreFrames);
        Log("restore present armed hold=%d settle=%d model=0x%08X tbl2=0x%08X handler=0x%08X",
            g_restoreFlipHoldBudget,
            g_restoreFlipSettleBudget,
            *MODEL_DIRECT_PTR,
            *(volatile u32*)WEAPON_MODEL_TABLE2,
            *WEAPON_HANDLER_PTR);
        Log("restore: no dispatch redirect — slot=%d equip=%d item=%d dispatch=%02X/%02X model=0x%08X handler=0x%08X",
            g_savedSlot, g_savedEquipId, g_savedItemId,
            *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, *MODEL_DIRECT_PTR, g_savedHandlerPtr);
    }

    if (g_knifeActive)
    {
        u8 stateNow = *DISPATCHER_STATE;
        u8 subNow   = *DISPATCHER_SUBSTATE;
        u8 seqNow   = *DISPATCHER_SEQ71;
        BOOL knifeVisualReadyNow = IsGameplayKnifeVisualReady();

        /* Re-assert knife state each frame (game may overwrite via SyncEquip or other paths). */
        *PL_ACTIVE_SLOT    = g_inventoryLatched ? (u8)(KNIFE_SLOT + 1) : g_savedSlot;
        *PL_EQUIP_ID       = KNIFE_ID;
        if (g_inventoryLatched || knifeVisualReadyNow)
            *WEAPON_HANDLER_PTR = KNIFE_HANDLER_ADDR;
        else
            *WEAPON_HANDLER_PTR = g_savedHandlerPtr;

        s_knifeHeldDiagFrame++;
        if (s_knifeHeldDiagFrame <= 30)
        {
            Log("held[%d]: slot=%d equip=%d dirty=0x%02X handler=0x%08X model=0x%08X tbl2=0x%08X dispatch=%02X/%02X pkan=0x%08X pseq=0x%08X key=0x%04X",
                s_knifeHeldDiagFrame, *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *WEAPON_DIRTY_FLAG,
                *WEAPON_HANDLER_PTR, *MODEL_DIRECT_PTR, *(volatile u32*)WEAPON_MODEL_TABLE2,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, *PL_PKAN, *PL_PSEQ, *G_KEY);
        }

        if (g_knifeReleasing)
        {
            /* RB was released mid-swing. Keep KEY_READY held so the attack animation
               runs to completion. Do NOT inject TRG or retrigger — we are draining only.
               When dispatch returns to idle (13/01) or the cap is hit, execute the restore. */
            g_swingDrainFrames++;
            BOOL swingDone = (stateNow == 0x13 && subNow == 1) ||
                             (g_swingDrainFrames >= 90);
            *G_KEY |= (u16)KEY_READY;
            Log("SwingDrain[%d]: dispatch=%02X/%02X seq=%02X done=%d",
                g_swingDrainFrames, stateNow, subNow, seqNow, swingDone ? 1 : 0);
            if (swingDone)
            {
                /* Commit the full release that KnifeThread deferred.
                   Only restore inventory state here — do NOT write PL_EQUIP_ID or
                   WEAPON_DIRTY_FLAG. g_needRestoreLoad handles those on the next frame.
                   Writing dirty flag here AND in g_needRestoreLoad causes two consecutive
                   dispatch cycles which produces the LB-aim loop bug. */
                *PL_ACTIVE_SLOT = g_savedSlot;
                if (g_savedSlot > 0)
                {
                    G_ITEM_WORK[(g_savedSlot - 1) * 2]     = g_savedItemId;
                    G_ITEM_WORK[(g_savedSlot - 1) * 2 + 1] = g_savedItemMeta;
                }
                G_ITEM_WORK[KNIFE_SLOT * 2]     = 0;
                G_ITEM_WORK[KNIFE_SLOT * 2 + 1] = 0;

                g_knifeReleasing        = FALSE;
                g_swingDrainFrames      = 0;
                g_knifeActive           = FALSE;
                g_entryFlipHoldBudget   = 0;
                g_entryFlipSettleBudget = 0;
                /* Arm suppress budget NOW so the gap frame between swingDone and
                   the g_needRestoreLoad one-shot (next frame) is also covered. */
                g_restoreFlipHoldBudget = 8;
                g_restoreFlipSettleBudget = 0;
                g_restoreFlipLogBudget  = 0;
                g_completedRbCycle      = g_activeRbCycle;
                g_postRbObserveArmed    = TRUE;
                g_postRbStableLogged    = FALSE;
                g_postRbReadyLogBudget  = 0;
                g_ddPresentLogBudget    = 0;
                g_tryLateGameplayLwm    = FALSE;
                g_lateGameplayLwmDone   = FALSE;
                g_tryLateRestoreLwm     = TRUE;
                g_lateRestoreLwmDone    = FALSE;
                g_restoreHookWakeTrigSent = FALSE;
                g_uiKnifePreviewActive  = FALSE;
                g_needRestoreLoad       = TRUE;
                g_aimBlockFrames        = 8;
                Log("SwingDrain: complete — restore armed drainFrames=%d cap=%d dispatch=%02X/%02X",
                    g_swingDrainFrames, (g_swingDrainFrames >= 90) ? 1 : 0,
                    stateNow, subNow);
            }
        }
        else
        {
            if (stateNow == 0x14 || (stateNow == 0x13 && subNow == 0) || seqNow == 0x09 || seqNow == 0x06)
                s_needKnifeReadyRetrigger = TRUE;

            *G_KEY |= (u16)KEY_READY;
            if (!s_prevKnifeActive)
                *G_KEY_TRG |= (u16)KEY_READY;
            else if (s_needKnifeReadyRetrigger &&
                     stateNow == 0x13 &&
                     subNow == 1 &&
                     seqNow == 0x0A)
            {
                *G_KEY_TRG |= (u16)KEY_READY;
                s_needKnifeReadyRetrigger = FALSE;
                if (s_readyRetriggerLogBudget < 8)
                {
                    s_readyRetriggerLogBudget++;
                    Log("Ready retrigger[%d]: dispatch=%02X/%02X seq=%02X frame72=%02X frame73=%02X key=0x%04X",
                        s_readyRetriggerLogBudget,
                        stateNow, subNow, seqNow,
                        *DISPATCHER_FRAME72, *DISPATCHER_FRAME73, *G_KEY);
                }
            }
        }
        s_prevKnifeActive = TRUE;
    }
    else
    {
        s_knifeHeldDiagFrame = 0;
        s_prevKnifeActive = FALSE;
        s_needKnifeReadyRetrigger = FALSE;
        s_readyRetriggerLogBudget = 0;

        /* Suppress weapon-action keys during restore window.
           Covers two phases:
           Phase 1 (LWM pending): block KEY_READY until LateRestoreLWM confirms model loaded.
           Phase 2 (settling): block both KEY_READY and KEY_AIM for g_aimBlockFrames more frames
           after LWM fires, so dispatch settles before player LB/aim input goes through.
           LB on this player maps to KEY_READY (0x0100), not KEY_AIM, so phase 2 must
           include KEY_READY to stop the pistol aim+fire animation loop. */
        if (!g_inventoryLatched &&
            ((g_tryLateRestoreLwm && !g_lateRestoreLwmDone) || g_aimBlockFrames > 0))
        {
            *G_KEY     &= ~(u16)KEY_READY;
            *G_KEY_TRG &= ~(u16)KEY_READY;
            *G_KEY     &= ~(u16)KEY_AIM;
            *G_KEY_TRG &= ~(u16)KEY_AIM;
        }
        if (g_aimBlockFrames > 0)
            g_aimBlockFrames--;

        u8 restoreStateNow = *DISPATCHER_STATE;
        u8 restoreSubNow = *DISPATCHER_SUBSTATE;
        BOOL postRbRestoreStable =
            g_postRbObserveArmed &&
            !g_inventoryLatched &&
            !g_needRestoreLoad &&
            !g_tryLateRestoreLwm &&
            g_lateRestoreLwmDone &&
            g_postRestoreFrames == 0 &&
            *PL_ACTIVE_SLOT == g_savedSlot &&
            *PL_EQUIP_ID == g_savedEquipId &&
            *MODEL_DIRECT_PTR == g_savedModelPtr1 &&
            *(volatile u32*)WEAPON_MODEL_TABLE2 == g_savedTbl2 &&
            *WEAPON_HANDLER_PTR == g_savedHandlerPtr;
        BOOL restoreHookWakeStateOk =
            ((restoreStateNow == 0x00 && restoreSubNow == 0x03) ||
             (restoreSubNow == 0x01 &&
              (restoreStateNow == 0x00 ||
               restoreStateNow == 0x02 ||
               restoreStateNow == 0x03 ||
               restoreStateNow == 0x04)));

        if (!g_inventoryLatched &&
            g_deferredKnifePress &&
            g_tryLateRestoreLwm &&
            !g_lateRestoreLwmDone &&
            restoreHookWakeStateOk &&
            *MODEL_DIRECT_PTR != g_savedModelPtr1)
        {
            char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
            ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
            *G_KEY |= (u16)KEY_READY;
            if (!g_restoreHookWakeTrigSent)
            {
                *G_KEY_TRG |= (u16)KEY_READY;
                g_restoreHookWakeTrigSent = TRUE;
                Log("Restore hook wake injected - slot=%d equip=%d model=0x%08X tbl2=0x%08X dispatch=%02X/%02X dbg=%s/%s/%s/%s",
                    *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *MODEL_DIRECT_PTR,
                    *(volatile u32*)WEAPON_MODEL_TABLE2,
                    restoreStateNow, restoreSubNow,
                    dbg1, dbg2, dbg3, dbg4);
            }
        }
        else
        {
            g_restoreHookWakeTrigSent = FALSE;
        }

        if (postRbRestoreStable && !g_postRbStableLogged)
        {
            g_postRbStableLogged = TRUE;
            Log("RB cycle[%d]: restore stable slot=%d equip=%d model=0x%08X tbl2=0x%08X handler=0x%08X dispatch=%02X/%02X first=%d hold=%d settle=%d",
                g_completedRbCycle,
                *PL_ACTIVE_SLOT,
                *PL_EQUIP_ID,
                *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2,
                *WEAPON_HANDLER_PTR,
                restoreStateNow,
                restoreSubNow,
                g_suppressNextFlip ? 1 : 0,
                g_entryFlipHoldBudget,
                g_entryFlipSettleBudget);
        }

        if (postRbRestoreStable &&
            (((*G_KEY & (u16)KEY_READY) != 0) || ((*G_KEY_TRG & (u16)KEY_READY) != 0)) &&
            !g_deferredKnifePress &&
            !g_restoreWakeTrigSent &&
            !g_restoreHookWakeTrigSent &&
            g_postRbReadyLogBudget < 12)
        {
            g_postRbReadyLogBudget++;
            Log("PostRBReady[%d]: cycle=%d key=0x%04X trg=0x%04X dispatch=%02X/%02X model=0x%08X tbl2=0x%08X handler=0x%08X first=%d hold=%d settle=%d",
                g_postRbReadyLogBudget,
                g_completedRbCycle,
                *G_KEY,
                *G_KEY_TRG,
                restoreStateNow,
                restoreSubNow,
                *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2,
                *WEAPON_HANDLER_PTR,
                g_suppressNextFlip ? 1 : 0,
                g_entryFlipHoldBudget,
                g_entryFlipSettleBudget);
        }

        if (g_postRestoreFrames > 0)
            g_postRestoreFrames--;

        /* If timer expired and LateRestoreLWM never fired (model already matched
           at restore time), force-complete so restorePendingNow clears and stops
           the deferred-press / KEY_READY injection loop. */
        if (g_postRestoreFrames == 0 && g_tryLateRestoreLwm && !g_lateRestoreLwmDone)
        {
            g_tryLateRestoreLwm  = FALSE;
            g_lateRestoreLwmDone = TRUE;
            Log("RestoreTimeout: forced complete — LateRestoreLWM never needed (model already matched)");
        }
    }
}

/* ================================================================
   HOOK INSTALL / UNINSTALL
   ================================================================ */
static void InstallMidHook(void)
{
    u8* site = HOOK_SITE;

    DWORD oldProt;
    if (!VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("Hook: VirtualProtect failed at 0x483527");
        return;
    }

    if (site[0] == 0xE9)
    {
        /* Already patched (e.g. another mod loaded first) — bail */
        VirtualProtect(site, 16, oldProt, &oldProt);
        Log("Hook: 0x483527 already has JMP, skipping");
        return;
    }

    int patchSize = DeterminePatchSize(site);
    if (patchSize == 0)
    {
        VirtualProtect(site, 16, oldProt, &oldProt);
        Log("Hook: could not determine patchSize — bytes 0x%02X 0x%02X 0x%02X",
            site[0], site[1], site[2]);
        return;
    }

    g_HookStub = (u8*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    if (!g_HookStub)
    {
        VirtualProtect(site, 16, oldProt, &oldProt);
        Log("Hook: VirtualAlloc failed");
        return;
    }

    /* Save original bytes for uninstall */
    memcpy(g_OrigBytes, site, patchSize);
    g_PatchSize = patchSize;

    /* Build stub: [original bytes] pushfd pushad call popad popfd jmp-back */
    u8* p = g_HookStub;
    memcpy(p, site, patchSize);  p += patchSize;
    *p++ = 0x9C;   /* pushfd */
    *p++ = 0x60;   /* pushad */
    *p++ = 0xE8;   /* call QuickKnifeInjectFn */
    *(s32*)p = (s32)((u8*)QuickKnifeInjectFn - (p + 4));  p += 4;
    *p++ = 0x61;   /* popad */
    *p++ = 0x9D;   /* popfd */
    *p++ = 0xE9;   /* jmp back to site+patchSize */
    *(s32*)p = (s32)((site + patchSize) - (p + 4));  p += 4;

    /* Patch site with JMP to stub, NOP the rest */
    *(site) = 0xE9;
    *(s32*)(site + 1) = (s32)(g_HookStub - (site + 5));
    for (int i = 5; i < patchSize; i++) site[i] = 0x90;

    VirtualProtect(site, 16, oldProt, &oldProt);
    g_hookInstalled = TRUE;
    Log("Hook: installed at 0x483527, patchSize=%d, stub=0x%08X",
        patchSize, (unsigned)g_HookStub);

    InstallLWMHook();
    InstallCommitHook();
    InstallPostStageHook();
    InstallDispatch0Hook();
    InstallFlipHook();
    InstallDDFlipHook();
}

static void UninstallMidHook(void)
{
    if (!g_hookInstalled || !g_HookStub) return;

    u8* site = HOOK_SITE;
    DWORD oldProt;
    VirtualProtect(site, 16, PAGE_EXECUTE_READWRITE, &oldProt);
    memcpy(site, g_OrigBytes, g_PatchSize);
    VirtualProtect(site, 16, oldProt, &oldProt);

    UninstallDDFlipHook();
    UninstallFlipHook();
    UninstallDispatch0Hook();
    UninstallPostStageHook();
    UninstallCommitHook();
    UninstallLWMHook();
    VirtualFree(g_HookStub, 0, MEM_RELEASE);
    g_HookStub = NULL;
    g_hookInstalled = FALSE;
    Log("Hook: uninstalled");
}

/* ================================================================
   THREAD
   Manages XInput state and press/release transitions.
   G_KEY injection is done synchronously by QuickKnifeInjectFn
   via the hook.  Falls back to polling injection if hook failed.
   ================================================================ */
static DWORD WINAPI KnifeThread(LPVOID unused)
{
    (void)unused;
    BOOL prevInventoryBtn = FALSE;
    BOOL prevKnifeBtn = FALSE;

    HMODULE hXInput = LoadLibraryA("xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!hXInput) hXInput = LoadLibraryA("xinput1_3.dll");

    if (hXInput)
    {
        g_XInputGetState = (XInputGetStateFn)GetProcAddress(hXInput, "XInputGetState");
        Log("XInput loaded: %s", g_XInputGetState ? "OK" : "GetProcAddress failed");
    }
    else
    {
        Log("XInput load FAILED");
    }

    /* Try to install hook now (Modsdk_post_init may not have run yet) */
    if (!g_hookInstalled)
        InstallMidHook();

    Log("Thread running — XInput: %s  Hook: %s",
        g_XInputGetState ? "OK" : "FAILED",
        g_hookInstalled  ? "OK" : "FAILED (polling fallback)");

    while (g_running)
    {
        BOOL knifeBtn = FALSE;
        BOOL inventoryBtn = FALSE;
        u16  padButtons = 0;
        BOOL restorePendingNow = FALSE;
        BOOL activateKnifeNow = FALSE;
        BOOL deferredPressLive = FALSE;
        BOOL acceptDeferredNow = FALSE;

        if (g_XInputGetState)
        {
            MY_XINPUT_STATE state;
            ZeroMemory(&state, sizeof(state));
            if (g_XInputGetState(0, &state) == 0)
            {
                padButtons = state.Gamepad.wButtons;
                knifeBtn = (padButtons & PAD_BTN_RB) != 0;
                inventoryBtn = (padButtons & PAD_BTN_Y) != 0;
            }
        }

        if (inventoryBtn && !prevInventoryBtn)
        {
            Log("Y pressed — buttons=0x%04X slot=%d equip=%d routine1=0x%02X dirty=0x%02X dispatch=%02X/%02X knifeActive=%d uiPreview=%d",
                padButtons, *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *PL_ROUTINE_1, *WEAPON_DIRTY_FLAG,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, g_knifeActive ? 1 : 0, g_uiKnifePreviewActive ? 1 : 0);
            g_inventoryLatched = !g_inventoryLatched;
            Log("Y latch - invLatched=%d slot=%d equip=%d routine1=0x%02X",
                g_inventoryLatched ? 1 : 0, *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *PL_ROUTINE_1);
        }
        else if (!inventoryBtn && prevInventoryBtn)
        {
            Log("Y released — buttons=0x%04X slot=%d equip=%d routine1=0x%02X dirty=0x%02X dispatch=%02X/%02X knifeActive=%d uiPreview=%d",
                padButtons, *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *PL_ROUTINE_1, *WEAPON_DIRTY_FLAG,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE, g_knifeActive ? 1 : 0, g_uiKnifePreviewActive ? 1 : 0);
        }

        restorePendingNow = (!g_inventoryLatched) &&
                            (g_needRestoreLoad ||
                             (g_tryLateRestoreLwm && !g_lateRestoreLwmDone) ||
                             (g_postRestoreFrames > 0 && !g_lateRestoreLwmDone));

        if (g_deferredKnifePressFrames > 0)
            g_deferredKnifePressFrames--;

        deferredPressLive = g_deferredKnifePress &&
                            (knifeBtn || g_deferredKnifePressFrames > 0);

        if (!deferredPressLive)
        {
            g_deferredKnifePress = FALSE;
            g_deferredKnifePressFrames = 0;
        }

        if (!restorePendingNow && g_deferredKnifePress && !knifeBtn)
        {
            g_deferredKnifePress = FALSE;
            g_deferredKnifePressFrames = 0;
        }

        if (!restorePendingNow || !g_deferredKnifePress)
            g_restoreWakeTrigSent = FALSE;

        if (!g_inventoryLatched &&
            !g_knifeActive &&
            deferredPressLive &&
            restorePendingNow)
        {
            char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
            ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
            *G_KEY |= (u16)KEY_READY;
            if (!g_restoreWakeTrigSent)
            {
                *G_KEY_TRG |= (u16)KEY_READY;
                g_restoreWakeTrigSent = TRUE;
                Log("Restore wake ready injected - slot=%d equip=%d model=0x%08X tbl2=0x%08X dispatch=%02X/%02X dbg=%s/%s/%s/%s",
                    *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *MODEL_DIRECT_PTR,
                    *(volatile u32*)WEAPON_MODEL_TABLE2,
                    *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                    dbg1, dbg2, dbg3, dbg4);
            }
        }

        if (!g_inventoryLatched &&
            !g_knifeActive &&
            g_deferredKnifePress &&
            knifeBtn &&
            !restorePendingNow &&
            *MODEL_DIRECT_PTR == g_savedModelPtr1 &&
            *(volatile u32*)WEAPON_MODEL_TABLE2 == g_savedTbl2 &&
            *WEAPON_HANDLER_PTR == g_savedHandlerPtr &&
            *PL_EQUIP_ID == g_savedEquipId &&
            *PL_ACTIVE_SLOT == g_savedSlot)
        {
            acceptDeferredNow = TRUE;
        }

        if (acceptDeferredNow)
        {
            char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
            ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
            g_deferredKnifePress = FALSE;
            g_deferredKnifePressFrames = 0;
            g_restoreWakeTrigSent = FALSE;
            activateKnifeNow = TRUE;
            Log("RB deferred press accepted - slot=%d equip=%d model=0x%08X tbl2=0x%08X dispatch=%02X/%02X dbg=%s/%s/%s/%s",
                *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                dbg1, dbg2, dbg3, dbg4);
        }

        if ((knifeBtn && !prevKnifeBtn) &&
            !g_inventoryLatched &&
            !g_knifeActive &&
            restorePendingNow)
        {
            char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
            ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
            g_deferredKnifePress = TRUE;
            g_deferredKnifePressFrames = 18;
            g_restoreWakeTrigSent = FALSE;
            g_restoreHookWakeTrigSent = FALSE;
            Log("RB ignored - restore pending need=%d tryLate=%d done=%d post=%d slot=%d equip=%d model=0x%08X savedModel=0x%08X tbl2=0x%08X savedTbl2=0x%08X dispatch=%02X/%02X dbg=%s/%s/%s/%s",
                g_needRestoreLoad ? 1 : 0,
                g_tryLateRestoreLwm ? 1 : 0,
                g_lateRestoreLwmDone ? 1 : 0,
                g_postRestoreFrames,
                *PL_ACTIVE_SLOT, *PL_EQUIP_ID,
                *MODEL_DIRECT_PTR, g_savedModelPtr1,
                *(volatile u32*)WEAPON_MODEL_TABLE2, g_savedTbl2,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                dbg1, dbg2, dbg3, dbg4);
        }

        if (!activateKnifeNow && knifeBtn && !prevKnifeBtn && !g_knifeActive)
            activateKnifeNow = TRUE;

        if (activateKnifeNow && !g_knifeActive)
        {
            if (restorePendingNow)
            {
                prevKnifeBtn = knifeBtn;
                prevInventoryBtn = inventoryBtn;
                Sleep(4);
                continue;
            }

            g_activeRbCycle = ++g_rbCycleCounter;
            g_knifeReleasing   = FALSE;
            g_swingDrainFrames = 0;
            g_postRbObserveArmed = FALSE;
            g_postRbStableLogged = FALSE;
            g_postRbReadyLogBudget = 0;
            g_ddPresentLogBudget = 0;
            Log("RB cycle[%d]: press start slot=%d equip=%d model=0x%08X tbl2=0x%08X handler=0x%08X dispatch=%02X/%02X first=%d hold=%d settle=%d post=%d",
                g_activeRbCycle,
                *PL_ACTIVE_SLOT,
                *PL_EQUIP_ID,
                *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2,
                *WEAPON_HANDLER_PTR,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                g_suppressNextFlip ? 1 : 0,
                g_entryFlipHoldBudget,
                g_entryFlipSettleBudget,
                g_postRestoreFrames);

            g_savedEquipId = *PL_EQUIP_ID;
            g_savedSlot    = *PL_ACTIVE_SLOT;
            g_savedHandlerPtr = *WEAPON_HANDLER_PTR;
            g_savedModelPtr1  = *MODEL_DIRECT_PTR;
            g_savedTbl2       = *(volatile u32*)WEAPON_MODEL_TABLE2;
            g_savedPkan       = *PL_PKAN;
            g_savedPseq       = *PL_PSEQ;
            g_pSinAtSave      = *PSIN_PARTS_PTR;
            memcpy(g_savedAnimBuffer, (const void*)WEAPON_MODEL_TABLE1, WEAPON_ANIM_BUFFER_SIZE);
            g_savedSlot0C     = (g_pSinAtSave > 0x400000u)
                                ? *(volatile u32*)(g_pSinAtSave + 14u * 124u + 0x0Cu) : 0;
            g_savedModelPtr2  = (g_pSinAtSave > 0x400000u)
                                ? *(volatile u32*)(g_pSinAtSave + 14u * 124u + 0x14u) : 0;
            if (g_pSinAtSave > 0x400000u)
                memcpy(g_savedPsin14, (const void*)(g_pSinAtSave + 14u * 124u), 124);
            g_savedC35271 = *(volatile u8*)0xC35271U;
            /* item ID in the previously active slot (used for restore visual) */
            g_savedItemId  = (g_savedSlot > 0)
                             ? G_ITEM_WORK[(g_savedSlot - 1) * 2]
                             : 0;
            g_savedItemMeta = (g_savedSlot > 0)
                              ? G_ITEM_WORK[(g_savedSlot - 1) * 2 + 1]
                              : 0;
            {
                char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
                ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
                Log("RB pressed — savedSlot=%d  savedItemId=%d dbg=%s/%s/%s/%s",
                    g_savedSlot, g_savedItemId, dbg1, dbg2, dbg3, dbg4);
            }

            if (g_inventoryLatched)
            {
                G_ITEM_WORK[KNIFE_SLOT * 2]     = KNIFE_ID;
                G_ITEM_WORK[KNIFE_SLOT * 2 + 1] = 1;
                *PL_ACTIVE_SLOT  = (u8)(KNIFE_SLOT + 1);
                *PL_EQUIP_ID     = KNIFE_ID;
                *WEAPON_DIRTY_FLAG = 0xFF;
                g_needEquipLoad = FALSE;
                g_needDispatchRedirect = FALSE;
                g_tryLateGameplayLwm = FALSE;
                g_lateGameplayLwmDone = FALSE;
                g_tryLateRestoreLwm = FALSE;
                g_lateRestoreLwmDone = FALSE;
                g_restoreHookWakeTrigSent = FALSE;
                Log("RB pressed - inventory-latched preview slot=%d equip=%d routine1=0x%02X",
                    *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *PL_ROUTINE_1);
            }
            else
            {
                if (g_savedSlot > 0)
                {
                    G_ITEM_WORK[(g_savedSlot - 1) * 2]     = KNIFE_ID;
                    G_ITEM_WORK[(g_savedSlot - 1) * 2 + 1] = 1;
                }
                *PL_ACTIVE_SLOT  = g_savedSlot;
                g_needEquipLoad  = TRUE;
                g_needDispatchRedirect = TRUE;   /* schedule one-shot LWM(knife) from weapon-update context */
                g_tryLateRestoreLwm = FALSE;
                g_lateRestoreLwmDone = FALSE;
                g_restoreHookWakeTrigSent = FALSE;
            }
            g_knifeActive    = TRUE;
        }
        else if (!knifeBtn && g_knifeActive && !g_knifeReleasing)
        {
            /* Mid-attack guard: if the knife swing is in progress (state 0x14),
               defer the release to QuickKnifeInjectFn so the animation finishes first. */
            if (!g_inventoryLatched && *DISPATCHER_STATE == 0x14)
            {
                g_knifeReleasing   = TRUE;
                g_swingDrainFrames = 0;
                Log("RB cycle[%d]: release mid-attack — drain armed dispatch=%02X/%02X",
                    g_activeRbCycle, *DISPATCHER_STATE, *DISPATCHER_SUBSTATE);
                prevKnifeBtn = knifeBtn;
                prevInventoryBtn = inventoryBtn;
                Sleep(4);
                continue;
            }

            char dbg1[5], dbg2[5], dbg3[5], dbg4[5];
            ReadDebugKeyStrings(dbg1, dbg2, dbg3, dbg4);
            Log("RB cycle[%d]: release start slot=%d equip=%d model=0x%08X tbl2=0x%08X handler=0x%08X dispatch=%02X/%02X first=%d hold=%d settle=%d post=%d",
                g_activeRbCycle,
                *PL_ACTIVE_SLOT,
                *PL_EQUIP_ID,
                *MODEL_DIRECT_PTR,
                *(volatile u32*)WEAPON_MODEL_TABLE2,
                *WEAPON_HANDLER_PTR,
                *DISPATCHER_STATE, *DISPATCHER_SUBSTATE,
                g_suppressNextFlip ? 1 : 0,
                g_entryFlipHoldBudget,
                g_entryFlipSettleBudget,
                g_postRestoreFrames);
            Log("RB released — restoring slot=%d  itemId=%d dbg=%s/%s/%s/%s",
                g_savedSlot, g_savedItemId, dbg1, dbg2, dbg3, dbg4);

            *PL_ACTIVE_SLOT                 = g_savedSlot;
            if (g_savedSlot > 0)
            {
                G_ITEM_WORK[(g_savedSlot - 1) * 2]     = g_savedItemId;
                G_ITEM_WORK[(g_savedSlot - 1) * 2 + 1] = g_savedItemMeta;
            }
            G_ITEM_WORK[KNIFE_SLOT * 2]     = 0;
            G_ITEM_WORK[KNIFE_SLOT * 2 + 1] = 0;
            *PL_EQUIP_ID = g_savedEquipId;
            *WEAPON_DIRTY_FLAG = 0xFF;
            if (g_inventoryLatched)
                *WEAPON_HANDLER_PTR = g_savedHandlerPtr;
            g_tryLateGameplayLwm = FALSE;
            g_lateGameplayLwmDone = FALSE;
            g_tryLateRestoreLwm = g_inventoryLatched ? FALSE : TRUE;
            g_lateRestoreLwmDone = FALSE;
            g_restoreHookWakeTrigSent = FALSE;
            g_uiKnifePreviewActive = FALSE;
            g_needRestoreLoad = g_inventoryLatched ? FALSE : TRUE;
            g_aimBlockFrames  = 20;
            g_knifeActive     = FALSE;
            g_entryFlipHoldBudget = 0;
            g_entryFlipSettleBudget = 0;
            g_completedRbCycle = g_activeRbCycle;
            g_postRbObserveArmed = TRUE;
            g_postRbStableLogged = FALSE;
            g_postRbReadyLogBudget = 0;
            g_ddPresentLogBudget = 0;
            Log("RB cycle[%d]: release cleared first=%d hold=%d settle=%d needRestore=%d tryLateRestore=%d post=%d",
                g_completedRbCycle,
                g_suppressNextFlip ? 1 : 0,
                g_entryFlipHoldBudget,
                g_entryFlipSettleBudget,
                g_needRestoreLoad ? 1 : 0,
                g_tryLateRestoreLwm ? 1 : 0,
                g_postRestoreFrames);
        }

        if (g_knifeActive)
        {
            if (g_inventoryLatched)
            {
                G_ITEM_WORK[KNIFE_SLOT * 2]     = KNIFE_ID;
                G_ITEM_WORK[KNIFE_SLOT * 2 + 1] = 1;
                *PL_ACTIVE_SLOT    = (u8)(KNIFE_SLOT + 1);
                *PL_EQUIP_ID       = KNIFE_ID;
                *WEAPON_DIRTY_FLAG = 0xFF;

                if (!g_uiKnifePreviewActive)
                {
                    g_uiKnifePreviewActive = TRUE;
                    Log("UI preview: forced hidden knife slot=%d equip=%d routine1=0x%02X",
                        *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *PL_ROUTINE_1);
                }
            }
            else
            {
                /* Re-assert slot every tick in case game overwrites it */
                *PL_ACTIVE_SLOT = g_savedSlot;

                if (g_uiKnifePreviewActive)
                {
                    g_uiKnifePreviewActive = FALSE;
                    Log("UI preview: returned to gameplay slot=%d equip=%d",
                        *PL_ACTIVE_SLOT, *PL_EQUIP_ID);
                }
            }

            /* Polling fallback if hook not installed */
            if (!g_hookInstalled)
            {
                *G_KEY     |= (u16)KEY_READY;
                *G_KEY_TRG |= (u16)KEY_READY;
            }
        }

        prevInventoryBtn = inventoryBtn;
        prevKnifeBtn = knifeBtn;

        Sleep(4);
    }

    Log("Thread exiting");
    return 0;
}

/* ================================================================
   SDK EXPORTS
   ================================================================ */
extern "C" {
    __declspec(dllexport) void Modsdk_init(void) {}

    __declspec(dllexport) void Modsdk_post_init(void)
    {
        Log("Modsdk_post_init — 0x483527[0]=0x%02X  hook=%d",
            HOOK_SITE[0], g_hookInstalled);
        if (!g_hookInstalled)
            InstallMidHook();

        /* Startup cleanup: clear hidden knife slot (slot 11) so leftover state
           from a previous crashed session doesn't persist in the save file. */
        G_ITEM_WORK[KNIFE_SLOT * 2]     = 0;
        G_ITEM_WORK[KNIFE_SLOT * 2 + 1] = 0;
        Log("Startup: PL_ACTIVE_SLOT=%d PL_EQUIP_ID=%d C35314=0x%08X",
            *PL_ACTIVE_SLOT, *PL_EQUIP_ID, *MODEL_DIRECT_PTR);
    }

    __declspec(dllexport) void Modsdk_close(void) { Log("Modsdk_close called"); }
    __declspec(dllexport) void Modsdk_load(unsigned char* src, size_t pos, size_t size) { (void)src; (void)pos; (void)size; }
    __declspec(dllexport) void Modsdk_save(unsigned char*& dst, size_t& size) { dst = NULL; size = 0; }
}

/* ================================================================
   DLL ENTRY
   ================================================================ */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        LogInit();
        Log("DLL_PROCESS_ATTACH");
        g_running = TRUE;
        g_thread  = CreateThread(NULL, 0, KnifeThread, NULL, 0, NULL);
        Log("Thread created: %s", g_thread ? "OK" : "FAILED");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        g_running = FALSE;
        if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
        if (g_knifeActive)
        {
            *PL_ACTIVE_SLOT = g_savedSlot;
            if (g_savedSlot > 0)
            {
                G_ITEM_WORK[(g_savedSlot - 1) * 2]     = g_savedItemId;
                G_ITEM_WORK[(g_savedSlot - 1) * 2 + 1] = g_savedItemMeta;
            }
        }
        UninstallMidHook();
        LogClose();
    }
    return TRUE;
}
