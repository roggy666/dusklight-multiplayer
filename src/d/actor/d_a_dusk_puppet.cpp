/**
 * @file d_a_dusk_puppet.cpp
 * Dusklight multiplayer remote-player puppet actor (Path B).
 *
 * Renders the human Link body model at each remote player's transform. Model
 * lifecycle pattern mirrors d_a_obj_TvCdlst. The manager daDuskPuppet_updateAll()
 * spawns/despawns one puppet per remote player based on dusk::net state.
 */

#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_dusk_puppet.h"
#include "d/actor/d_a_player.h"        // daPy_getLinkPlayerActorClass()
#include "d/actor/d_a_horse.h"         // daHorse_c::getRootMtx() (passenger seat)
#include "d/d_com_inf_game.h"
#include "d/d_event_manager.h"        // dEvent_manager_c::getRunEventName (cutscene sync)
#include "d/d_kankyo.h"               // g_env_light
#include "m_Do/m_Do_ext.h"            // mDoExt_J3DModel__create / mDoExt_modelUpdateDL
#include "m_Do/m_Do_mtx.h"            // mDoMtx_stack_c
#include "f_pc/f_pc_layer.h"          // fpcLy_CurrentLayer / fpcLy_SetCurrentLayer
#include "f_pc/f_pc_manager.h"        // fpcM_SearchByName
#include "f_pc/f_pc_node.h"           // process_node_class
#include "f_op/f_op_overlap_mng.h"    // fopOvlpM_IsDoingReq (loading guard)
#include "m_Do/m_Do_dvd_thread.h"     // mDoDvdThd_mountArchive_c
#include "d/d_resorce.h"              // dRes_info_c::loaderBasicBmd
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/J3DGraphLoader/J3DAnmLoader.h"   // J3DAnmLoaderDataBase
#include "JSystem/J3DGraphAnimator/J3DAnimation.h"  // J3DAnmTexPattern (face BTP)
#include "JSystem/J3DGraphAnimator/J3DModelData.h"  // newSharedDisplayList / makeSharedDL
#include "JSystem/J3DGraphAnimator/J3DJoint.h"       // J3DJoint::getJntNo (region MtxCalc)
#include "JSystem/J3DGraphBase/J3DTransform.h"      // j3dDefaultMtx, J3DMtxCalc bases
#include "JSystem/J3DGraphBase/J3DSys.h"            // j3dSys
#include "res/Object/AlAnm.h"                      // dRes_ID_ALANM_BCK_/BTP_* (Link anims)

#include "dusk/net.h"
#include "dusk/logging.h"
#include "d/d_item_data.h"            // dItemNo_WEAR_* (costume -> model archive)

#include <cstring>                    // strlen/strcmp/strstr (archive file-name scan)

static const u32 l_puppetHeapSize = 0x40000;  // body + head + hands J3DModels

// --- Independent per-costume Link models ------------------------------------
// We must NOT draw Link's LIVE shared body model: his actor mutates its
// materials/skin every frame, and rendering that same instance outside his
// pipeline corrupts GPU state (hang). Instead we mount our OWN copy of each
// costume's model archive and parse al.bmd into a private, clean J3DModelData
// that all puppets of that costume share between themselves (never with Link).
// A remote player's costume (dItemNo_WEAR_*) is synced over the net and selects
// which model set the puppet uses. Each archive is mounted once, async, lazily.
namespace {

// One model set per Link outfit.
enum {
    kCostumeCasual = 0,  // Ordon clothes (start of game)
    kCostumeHero,        // green hero / Kokiri tunic
    kCostumeZora,        // Zora armor
    kCostumeMagic,       // magic armor
    kCostumeCount,
};

// Game-data archive per costume slot. All are assumed to share Link's body
// file-naming convention (al.bmd / al_head.bmd / al_hands.bmd); if a costume
// archive uses different names, loadPart() logs it and that slot stays empty
// (the puppet just won't spawn for that costume — no crash).
const char* const kCostumeArc[kCostumeCount] = {
    "/res/Object/Bmdl.arc",  // casual / Ordon
    "/res/Object/Kmdl.arc",  // hero (known good)
    "/res/Object/Zmdl.arc",  // zora
    "/res/Object/Mmdl.arc",  // magic armor
};

struct CostumeModels {
    mDoDvdThd_mountArchive_c* mount  = nullptr;
    J3DModelData*             body   = nullptr;  // al.bmd (mandatory)
    J3DModelData*             head   = nullptr;  // al_head.bmd  (hair/head; optional)
    J3DModelData*             face   = nullptr;  // al_face.bmd  (eyes/mouth; optional)
    J3DModelData*             hands  = nullptr;  // al_hands.bmd (optional)
    bool                      ready  = false;
    bool                      failed = false;
    bool                      faceApplied = false;  // face BTP registered on head data
};
CostumeModels s_costume[kCostumeCount];

// Map a synced clothes item id to a costume slot (defaults to hero).
int costumeSlotFor(u8 clothesId) {
    switch (clothesId) {
        case dItemNo_WEAR_CASUAL_e: return kCostumeCasual;
        case dItemNo_WEAR_ZORA_e:   return kCostumeZora;
        case dItemNo_ARMOR_e:       return kCostumeMagic;
        case dItemNo_WEAR_KOKIRI_e:
        default:                    return kCostumeHero;
    }
}

// Parse one BMD out of a mounted archive into an independent, fully initialized
// J3DModelData. type==0 -> flat by-name search (finds the file in any subdir).
// loaderBasicBmd allocations go to the current heap, so we point it at the
// archive heap.
J3DModelData* loadPart(JKRMemArchive* arc, const char* name, const char* arcName) {
    void* raw = arc->getResource(0u, name);
    if (raw == nullptr) {
        DuskLog.warn("puppet: {} not found in {}", name, arcName);
        return nullptr;
    }
    JKRHeap* prev = mDoExt_getArchiveHeap()->becomeCurrentHeap();
    J3DModelData* data = dRes_info_c::loaderBasicBmd('BMDR', raw);
    if (prev != nullptr) prev->becomeCurrentHeap();
    return data;
}

// Loads one costume's body/head/hands models once (async mount). True when that
// slot is ready. Body is mandatory; head/hands are best-effort.
bool ensureCostumeReady(int slot) {
    if (slot < 0 || slot >= kCostumeCount) return false;
    CostumeModels& c = s_costume[slot];
    if (c.ready)  return true;
    if (c.failed) return false;

    const char* arcName = kCostumeArc[slot];
    if (c.mount == nullptr) {
        c.mount = mDoDvdThd_mountArchive_c::create(arcName, 0, nullptr);
        if (c.mount == nullptr) { c.failed = true; DuskLog.warn("puppet: {} mount failed", arcName); }
        return false;  // mount started; check again next frame
    }
    if (c.mount->sync() == 0) return false;  // still mounting

    JKRMemArchive* arc = c.mount->getArchive();
    if (arc == nullptr) { c.failed = true; DuskLog.warn("puppet: {} archive null", arcName); return false; }

    // Resolve the body/head/hands .bmd names. Kmdl uses al.bmd / al_head.bmd /
    // al_hands.bmd; other costume archives use a different prefix (e.g. Bmdl uses
    // "bl"). So if al.bmd is absent we find the "<prefix>_head.bmd" file, take its
    // prefix, and derive body = "<prefix>.bmd", hands = "<prefix>_hands.bmd". This
    // self-adapts to any costume archive and avoids grabbing unrelated models
    // (e.g. al_kantera.bmd, the lantern) as the body.
    const char* bodyNm  = "al.bmd";
    const char* headNm  = "al_head.bmd";
    const char* handsNm = "al_hands.bmd";
    // The FACE model is identity, shared across costumes (only the clothes/body
    // change), so it's always named "al_face.bmd" even in the bl/zl/ml archives.
    const char* faceNm  = "al_face.bmd";
    char bodyBuf[64];
    char handsBuf[64];
    if (arc->getResource(0u, bodyNm) == nullptr) {
        bodyNm = headNm = handsNm = nullptr;
        const u32 nf = arc->countFile();
        for (u32 i = 0; i < nf; ++i) {
            const JKRArchive::SDIFileEntry& e = arc->mFiles[i];
            if (e.isDirectory()) continue;
            const char* nm = arc->mStringTable + e.getNameOffset();
            const char* hp = std::strstr(nm, "_head.bmd");
            if (hp == nullptr) continue;
            const size_t pl = (size_t)(hp - nm);  // prefix length, e.g. "bl"
            if (pl == 0 || pl >= sizeof(bodyBuf) - 12) break;
            headNm = nm;
            std::memcpy(bodyBuf, nm, pl);  std::strcpy(bodyBuf + pl, ".bmd");        bodyNm  = bodyBuf;
            std::memcpy(handsBuf, nm, pl); std::strcpy(handsBuf + pl, "_hands.bmd"); handsNm = handsBuf;
            break;
        }
        DuskLog.info("puppet: {} scan -> body={} head={} face={} hands={}", arcName,
                     bodyNm ? bodyNm : "(none)", headNm ? headNm : "(none)",
                     faceNm, handsNm ? handsNm : "(none)");
    }
    c.body  = bodyNm  ? loadPart(arc, bodyNm,  arcName) : nullptr;
    c.head  = headNm  ? loadPart(arc, headNm,  arcName) : nullptr;
    c.face  = faceNm  ? loadPart(arc, faceNm,  arcName) : nullptr;
    c.hands = handsNm ? loadPart(arc, handsNm, arcName) : nullptr;
    if (c.body == nullptr) { c.failed = true; return false; }

    c.ready = true;
    DuskLog.info("puppet: costume {} ({}) ready body={} head={} face={} hands={}",
                 slot, arcName, (void*)c.body, (void*)c.head, (void*)c.face, (void*)c.hands);
    return true;
}

// Create a bind-pose McaMorf wrapper for a model part (null model -> null).
mDoExt_McaMorf* makePartMorf(J3DModelData* data) {
    if (data == nullptr) return nullptr;
    mDoExt_McaMorf* m = JKR_NEW mDoExt_McaMorf(data, NULL, NULL, NULL, 2, 1.0f, 0, -1, 1,
                                               NULL, 0x80000, 0x11000284);
    if (m == nullptr || m->getModel() == nullptr) return nullptr;
    return m;
}

// Body-animation cache. Each remote player's current BCK id is synced over the
// net; the puppet loads that BCK on demand and plays it. Animations share Link's
// skeleton, so they apply to any costume model. Each BCK is a PRIVATE,
// decompressed copy — never Link's live anm resource, which
// J3DAnmLoaderDataBase::setResource patches in place and would corrupt Link's
// own animations (the local player glitched / "died" mid-run).
constexpr u16 kAnimNone     = 0xFFFF;
constexpr int kAnimCacheMax = 64;
struct AnimCacheEntry { u16 id; J3DAnmTransform* anm; };
AnimCacheEntry s_animCache[kAnimCacheMax];
int            s_animCacheCount = 0;
JKRArchive*    s_animCacheArc   = nullptr;  // arc the cache was built from (invalidate on change)
u8*            s_scratch        = nullptr;  // reused read buffer (on the volatile archive heap)

// Read a resource into a fresh, persistent, decompressed private buffer. Never
// hand a live archive resource to the J3D loaders — they patch it in place and
// would corrupt Link's shared anm data (the local player glitched mid-run).
void* readResCopy(JKRArchive* arc, u16 resId) {
    const u32 kCap = 0x8000;  // 32 KiB scratch, reused; ample for one BCK/BTP
    JKRHeap* heap = mDoExt_getArchiveHeap();
    if (s_scratch == nullptr) {
        s_scratch = static_cast<u8*>(heap->alloc(kCap, 0x20));
        if (s_scratch == nullptr) return nullptr;
    }
    const u32 n = JKRReadIdxResource(s_scratch, kCap, resId, arc);  // decompress + copy full
    if (n == 0) return nullptr;
    void* buf = heap->alloc(n, 0x20);  // right-sized persistent copy
    if (buf == nullptr) return nullptr;
    std::memcpy(buf, s_scratch, n);
    return buf;
}

J3DAnmTransform* loadBckCopy(JKRArchive* anm, u16 resId) {
    void* buf = readResCopy(anm, resId);
    if (buf == nullptr) return nullptr;
    JKRHeap* prev = mDoExt_getArchiveHeap()->becomeCurrentHeap();
    mDoExt_transAnmBas* t = JKR_NEW mDoExt_transAnmBas(NULL);  // NULL bas: no sound triggers
    if (t != nullptr) J3DAnmLoaderDataBase::setResource(t, buf);
    if (prev != nullptr) prev->becomeCurrentHeap();
    return t;
}

// Give the face model its eyes/mouth: Link's face is a separate model
// (al_face.bmd / bl_face.bmd) whose face texture is chosen by a texture-pattern
// anim (BTP_FA) — without it the face is blank. The puppet models use a baked
// single DL (loaderBasicBmd -> makeSharedDL), so a runtime BTP can't change the
// texture; we register it and re-bake with the face texNo selected. Done once per
// shared face model data. Mirrors daAlink_c::setFaceBtp + the loader's DL bake.
bool bakeFaceTextures(J3DModelData* faceData) {
    if (faceData == nullptr) return false;
    JKRArchive* anm = dComIfGp_getAnmArchive();
    if (anm == nullptr) return false;  // archive not ready yet; caller retries
    void* buf = readResCopy(anm, dRes_ID_ALANM_BTP_FA_e);
    if (buf == nullptr) { DuskLog.warn("puppet: face BTP read failed"); return false; }

    JKRHeap* prev = mDoExt_getArchiveHeap()->becomeCurrentHeap();
    bool ok = false;
    J3DAnmTexPattern* btp = (J3DAnmTexPattern*)J3DAnmLoaderDataBase::load(buf);
    if (btp != nullptr) {
        btp->searchUpdateMaterialID(faceData);
        faceData->entryTexNoAnimator(btp);
        btp->setFrame(0.0f);
        // Re-bake so the BTP-selected face texNo is captured in the shared DL.
        if (faceData->newSharedDisplayList(J3DMdlFlag_UseSingleDL) == kJ3DError_Success) {
            faceData->simpleCalcMaterial(const_cast<MtxP>(j3dDefaultMtx));
            faceData->makeSharedDL();
            ok = true;
            DuskLog.info("puppet: face textures baked");
        }
    }
    if (prev != nullptr) prev->becomeCurrentHeap();
    if (!ok) DuskLog.warn("puppet: face bake failed");
    return ok;
}

// Cached lookup; loads (and caches, including failures) one BCK per resource id.
J3DAnmTransform* getAnimById(u16 resId) {
    if (resId == kAnimNone) return nullptr;
    JKRArchive* anm = dComIfGp_getAnmArchive();
    if (anm == nullptr) return nullptr;  // archive not ready yet; retry (don't cache)
    // The cached BCK copies live on the (volatile) archive heap and the anm
    // archive itself reloads on scene/cutscene transitions. When that happens the
    // cached pointers dangle -> using them crashes. Detect the reload (archive
    // pointer changed) and drop the whole cache + scratch so we reload fresh.
    if (anm != s_animCacheArc) {
        s_animCacheCount = 0;
        s_scratch        = nullptr;  // old buffer was on the freed heap
        s_animCacheArc   = anm;
    }
    for (int i = 0; i < s_animCacheCount; ++i) {
        if (s_animCache[i].id == resId) return s_animCache[i].anm;
    }
    if (s_animCacheCount >= kAnimCacheMax) return nullptr;  // cache full: keep current anim
    J3DAnmTransform* a = loadBckCopy(anm, resId);
    s_animCache[s_animCacheCount].id  = resId;  // cache result (even null) to avoid re-loading
    s_animCache[s_animCacheCount].anm = a;
    s_animCacheCount++;
    if (a == nullptr) DuskLog.warn("puppet: anim {} load failed", (int)resId);
    return a;
}

// Crossfade tuning. A layer's anim switch blends over kMorphFrames render frames
// (~0.13 s at 60 fps): long enough to kill the idle->run "arm teleport", short
// enough not to feel like the arms lag the body.
constexpr int kMorphFrames = 8;
constexpr f32 kMorphStep   = 1.0f / (f32)kMorphFrames;

// Advance one layer's morf progress and return the INCREMENTAL blend factor for
// this frame: the fraction of the still-remaining gap to close now. With cur
// ramping linearly 0->1, (cur-prev)/(1-prev) eases the stored pose onto the live
// target and reaches it exactly when cur hits 1 (mirrors mDoExt_morf_c). Returns
// 1.0 (snap) when the layer isn't crossfading.
static f32 advanceMorf(f32& cur, bool& active) {
    if (!active) return 1.0f;
    const f32 prev = cur;
    cur += kMorphStep;
    if (cur >= 1.0f) { cur = 1.0f; active = false; }
    const f32 denom = 1.0f - prev;
    if (denom <= 0.0001f) return 1.0f;
    f32 f = (cur - prev) / denom;
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
    return f;
}

// Per-joint region animation: Link's body is a blend of a lower-body (legs/
// locomotion) anim and an upper-body (arms/torso) anim. The stock blend table is
// a GLOBAL weighted lerp (would mash legs+arms together), so we drive the
// skeleton ourselves: upper-body joints (~ spine 2 .. end of arms 0x11; arms are
// chains 7-9 and 0xC-0xE, head is 4) take the upper anim; root/waist/legs take
// the under anim. Each frame the caller sets `under`/`upper` (already frame-
// advanced) plus the per-layer morf factors and the puppet's persistent `pose`
// cache; a throwaway instance is set as the body's MtxCalc for one calc().
//
// Crossfade: each joint eases its stored pose toward the live target by its
// layer's factor (1 = snap). Rotation is shortest-arc per-axis s16 (the s16 wrap
// of the delta picks the short way round); scale/translate are linear. The
// blended pose is fed through the SAME J3DMtxCalcCalcTransformMaya::calcTransform
// the non-morph path uses, so all the parent-stack / scale-compensate handling is
// identical — we never re-implement the engine's fragile setJ3DData.
struct PuppetBodyMtxCalc
    : public J3DMtxCalcNoAnm<J3DMtxCalcCalcTransformMaya, J3DMtxCalcJ3DSysInitMaya> {
    J3DAnmTransform*  under = nullptr;
    J3DAnmTransform*  upper = nullptr;
    J3DTransformInfo* pose  = nullptr;   // -> puppet mPose[kMaxJoints]
    f32               underFactor = 1.0f;
    f32               upperFactor = 1.0f;
    virtual ~PuppetBodyMtxCalc() {}
    virtual void calc() {
        const u16 jntNo = getJoint()->getJntNo();
        j3dSys.setCurrentMtxCalc(this);
        // Spine/head/arms (joints 2..0x11) follow the upper (arms) anim; root/
        // waist/legs follow the under (locomotion) anim.
        const bool upperRegion = (jntNo >= 2 && jntNo <= 0x11);
        J3DAnmTransform* a = (upperRegion && upper != nullptr) ? upper : under;
        if (a == nullptr) a = (under != nullptr) ? under : upper;
        if (a == nullptr) return;
        J3DTransformInfo target;
        a->getTransform(jntNo, &target);

        const f32 f = upperRegion ? upperFactor : underFactor;
        // Snap (and seed the pose cache) when not crossfading, or when this joint
        // is beyond the cache (defensive: Link's body fits, but never index OOB).
        if (pose == nullptr || jntNo >= daDuskPuppet_c::kMaxJoints || f >= 1.0f) {
            if (pose != nullptr && jntNo < daDuskPuppet_c::kMaxJoints) pose[jntNo] = target;
            J3DMtxCalcCalcTransformMaya::calcTransform(target);
            return;
        }
        // Ease the stored pose a fraction `f` toward the live target this frame.
        J3DTransformInfo& p = pose[jntNo];
        p.mScale.x     += (target.mScale.x     - p.mScale.x)     * f;
        p.mScale.y     += (target.mScale.y     - p.mScale.y)     * f;
        p.mScale.z     += (target.mScale.z     - p.mScale.z)     * f;
        p.mTranslate.x += (target.mTranslate.x - p.mTranslate.x) * f;
        p.mTranslate.y += (target.mTranslate.y - p.mTranslate.y) * f;
        p.mTranslate.z += (target.mTranslate.z - p.mTranslate.z) * f;
        const s16 dx = (s16)(target.mRotation.x - p.mRotation.x);  // s16 wrap = short arc
        const s16 dy = (s16)(target.mRotation.y - p.mRotation.y);
        const s16 dz = (s16)(target.mRotation.z - p.mRotation.z);
        p.mRotation.x = (s16)(p.mRotation.x + (s16)(dx * f));
        p.mRotation.y = (s16)(p.mRotation.y + (s16)(dy * f));
        p.mRotation.z = (s16)(p.mRotation.z + (s16)(dz * f));
        J3DMtxCalcCalcTransformMaya::calcTransform(p);
    }
};
}  // namespace

int daDuskPuppet_c::CreateHeap() {
    const CostumeModels& c = s_costume[mSlot];
    if (c.body == NULL) {
        return 0;  // manager only spawns once the body is ready, but be safe
    }
    // A skinned character model must have a joint MtxCalc driving it (McaMorf
    // sets itself as the joints' MtxCalc in modelCalc); a raw J3DModel::calc()
    // reuses whatever MtxCalc the previous actor left in j3dSys -> garbage skin
    // matrices -> GPU hang. NULL animation = bind pose.
    mpBody = makePartMorf(c.body);
    if (mpBody == NULL) {
        return 0;
    }
    mpHead  = makePartMorf(c.head);   // optional (al_head.bmd: hair/head)
    mpFace  = makePartMorf(c.face);   // optional (al_face.bmd: eyes/mouth)
    // The body model already carries Link's hands, so we do NOT draw the separate
    // hands model — doing so duplicates them ("too many hands"). Kept loadable in
    // case a future pose needs it, but not instantiated on the puppet.
    mpHands = NULL;
    return 1;
}

int daDuskPuppet_c::createHeapCallBack(fopAc_ac_c* i_this) {
    return static_cast<daDuskPuppet_c*>(i_this)->CreateHeap();
}

void daDuskPuppet_c::setBaseMtx() {
    mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
    mDoMtx_stack_c::ZXYrotM(0, shape_angle.y, 0);
    J3DModel* model = mpBody->getModel();
    model->setBaseScale(scale);
    model->setBaseTRMtx(mDoMtx_stack_c::get());
}

int daDuskPuppet_c::create() {
    fopAcM_ct(this, daDuskPuppet_c);
    const u32 prm = fopAcM_GetParam(this);
    mRemoteId = (u8)(prm & 0xFF);          // low byte: net player id
    mSlot     = (u8)((prm >> 8) & 0xFF);   // next byte: costume slot
    if (mSlot >= kCostumeCount) mSlot = kCostumeHero;
    mpBody = mpHead = mpFace = mpHands = NULL;
    mAnimStarted = false;
    mCurAnimId  = kAnimNone;
    mCurUpperId = kAnimNone;
    mUnderFrame = 0.0f;
    mUpperFrame = 0.0f;
    mMorphReady = false;            // first calc seeds mPose; no crossfade until then
    mUnderMorf = mUpperMorf = 1.0f; // settled
    mUnderMorfing = mUpperMorfing = false;
    scale.set(1.0f, 1.0f, 1.0f);

    // The costume's body model must already be loaded (the manager only spawns
    // puppets once ensureCostumeReady() is true for that slot).
    if (s_costume[mSlot].body == NULL) {
        return cPhs_ERROR_e;
    }
    if (fopAcM_entrySolidHeap(this, createHeapCallBack, l_puppetHeapSize) == 0) {
        return cPhs_ERROR_e;
    }
    setBaseMtx();
    fopAcM_SetMtx(this, mpBody->getModel()->getBaseTRMtx());
    fopAcM_setCullSizeBox2(this, mpBody->getModel()->getModelData());
    DuskLog.info("puppet id={} created (head={} hands={})", (int)mRemoteId,
                 mpHead != NULL, mpHands != NULL);
    return cPhs_COMPLEATE_e;
}

int daDuskPuppet_c::Execute() {
    if (mpBody == NULL) {
        return 1;
    }
    // The actor's Execute/Draw run during fpcM_Management — BEFORE the manager
    // (daDuskPuppet_updateAll) gets a chance to despawn us. So self-gate here on
    // the LIVE conditions: while the scene is streaming (archive heap being reset/
    // repopulated -> our cached anims/models are freed) or a cutscene/demo is
    // playing, skip all calc + draw. Otherwise we'd calc with dangling pointers
    // and crash (the cutscene crash). The manager despawns us shortly after.
    if (fopOvlpM_IsDoingReq() != 0 || !daDuskPuppet_remotesVisible()) {
        return 1;
    }
    const dusk::net::PlayerState* p = dusk::net::getRemotePlayerById(mRemoteId);
    // This player is in a cutscene/demo: skip (their demo anim ids aren't valid
    // here and would crash). The manager despawns us next frame.
    if (p != NULL && (p->flags & dusk::net::kFlagInCutscene) != 0) {
        return 1;
    }
    if (p != NULL) {
        // Smooth toward the latest reported transform instead of snapping: net
        // updates arrive at the relay tick rate (< render rate), so a hard snap
        // looks jerky even at 0 ping. Exponential interpolation per frame; but if
        // the target jumped far (warp / room change) snap so we don't slide across
        // the map.
        // While mounted, RIGIDLY attach the rider to the shared horse's seat instead
        // of chasing its network-smoothed Link pos — otherwise it lags and "flies
        // off" at speed. The horse itself is already position-synced (slaved), so we
        // just snap to its seat each frame and let the synced ride anim play. Seats
        // reuse the engine's Link+Zelda offsets: driver = front, passengers stack at
        // the rear. Origin at the horse GROUND Y so the ride anim lifts onto the seat.
        const bool onHorse = (p->flags & dusk::net::kFlagOnHorse) != 0;
        daHorse_c* horse = onHorse ? dComIfGp_getHorseActor() : NULL;
        if (horse != NULL) {
            static const Vec kFrontSeat = {-75.894f, 57.61f, 4.079f};  // driver
            static const Vec kRearSeat  = {-5.894f, 52.61f, 4.079f};   // passenger(s)
            cXyz seat;
            mDoMtx_multVec(horse->getRootMtx(),
                           dusk::net::isHorseDriver(p->id) ? &kFrontSeat : &kRearSeat, &seat);
            current.pos.set(seat.x, horse->current.pos.y, seat.z);
            shape_angle.y = horse->shape_angle.y;
        } else {
            const f32 dx = p->posX - current.pos.x;
            const f32 dy = p->posY - current.pos.y;
            const f32 dz = p->posZ - current.pos.z;
            const f32 dist2 = dx * dx + dy * dy + dz * dz;
            const f32 kSnapDist2 = 300.0f * 300.0f;  // > ~3 m: treat as a teleport
            if (dist2 > kSnapDist2) {
                current.pos.set(p->posX, p->posY, p->posZ);
                shape_angle.y = p->angleY;
            } else {
                const f32 k = 0.35f;  // smoothing factor (0..1); higher = snappier
                current.pos.x += dx * k;
                current.pos.y += dy * k;
                current.pos.z += dz * k;
                shape_angle.y += (s16)((s16)(p->angleY - shape_angle.y) * k);  // shortest-arc
            }
        }
    }
    setBaseMtx();

    // Drive the body from TWO synced layers: lower (legs/locomotion) + upper
    // (arms/torso), blended per joint by PuppetBodyMtxCalc so running legs and
    // swinging arms both play. Each layer falls back / resets on id change; the
    // lower layer falls back to the standby idle. Bind pose only if nothing loads.
    // Two synced layers: lower (legs/locomotion, UNDER_0) + upper (arms/torso,
    // UPPER_2), blended per joint by PuppetBodyMtxCalc. Reset a layer's frame when
    // its anim id changes; the lower layer falls back to the standby idle.
    u16 wantUnder = (p != NULL && p->anim != kAnimNone) ? p->anim
                                                        : (u16)dRes_ID_ALANM_BCK_WAITS_e;
    J3DAnmTransform* under = getAnimById(wantUnder);
    if (under == NULL && wantUnder != (u16)dRes_ID_ALANM_BCK_WAITS_e) {
        wantUnder = (u16)dRes_ID_ALANM_BCK_WAITS_e;
        under = getAnimById(wantUnder);
    }
    if (wantUnder != mCurAnimId) {
        mUnderFrame = 0.0f;
        mCurAnimId = wantUnder;
        // Start a lower-layer crossfade from the pose we're currently holding
        // (mPose already has the old anim's pose) toward the new anim. Skip on the
        // very first switch (pose cache not yet seeded -> would blend from garbage).
        if (mMorphReady) { mUnderMorf = 0.0f; mUnderMorfing = true; }
    }

    const u16 wantUpper = (p != NULL) ? p->animUpper : kAnimNone;
    J3DAnmTransform* upper = getAnimById(wantUpper);  // NULL when none/idle
    if (wantUpper != mCurUpperId) {
        mUpperFrame = 0.0f;
        mCurUpperId = wantUpper;
        if (mMorphReady) { mUpperMorf = 0.0f; mUpperMorfing = true; }
    }

    if (under != NULL) {
        mUnderFrame += 1.0f;
        const f32 mx = under->getFrameMax();
        if (mx > 0.0f && mUnderFrame >= mx) mUnderFrame -= mx;
        under->setFrame(mUnderFrame);
        mAnimStarted = true;
    }
    if (upper != NULL) {
        mUpperFrame += 1.0f;
        const f32 mx = upper->getFrameMax();
        if (mx > 0.0f && mUpperFrame >= mx) mUpperFrame -= mx;
        upper->setFrame(mUpperFrame);
    }

    if (mAnimStarted && under != NULL) {
        // Advance each layer's crossfade once per frame (the MtxCalc::calc below
        // runs per joint, so factors must be computed here). Returns 1.0 = snap.
        const f32 underFactor = advanceMorf(mUnderMorf, mUnderMorfing);
        const f32 upperFactor = advanceMorf(mUpperMorf, mUpperMorfing);

        PuppetBodyMtxCalc mc;  // throwaway: valid for this calc() only
        mc.under = under;
        mc.upper = upper;
        mc.pose  = mPose;
        mc.underFactor = underFactor;
        mc.upperFactor = upperFactor;
        J3DModel* bm = mpBody->getModel();
        bm->getModelData()->getJointNodePointer(0)->setMtxCalc(&mc);
        bm->calc();
        mMorphReady = true;  // mPose is now seeded -> subsequent switches crossfade
    }

    // Attach head + hands to the body's joints (mirrors daAlink_c draw):
    // head/face hang off joint 4; the hands model's joints 1/2 follow the body's
    // wrist joints 9 and 0xE.
    J3DModel* body = mpBody->getModel();
    if (mpHead != NULL) {  // hair/head: hangs off body joint 4
        mpHead->getModel()->setBaseTRMtx(body->getAnmMtx(4));
        mpHead->modelCalc();
    }
    if (mpFace != NULL) {  // eyes/mouth: also hangs off joint 4 (mirrors daAlink_c)
        // Bake the default face texture-pattern into the shared face model once
        // (anm archive may not be ready until the first in-world frames, so retry).
        if (!s_costume[mSlot].faceApplied && bakeFaceTextures(s_costume[mSlot].face)) {
            s_costume[mSlot].faceApplied = true;
        }
        mpFace->getModel()->setBaseTRMtx(body->getAnmMtx(4));
        mpFace->modelCalc();
    }
    if (mpHands != NULL) {
        J3DModel* hands = mpHands->getModel();
        hands->setBaseTRMtx(body->getBaseTRMtx());
        mpHands->modelCalc();
        hands->setAnmMtx(1, body->getAnmMtx(9));
        hands->setAnmMtx(2, body->getAnmMtx(0xE));
    }
    return 1;
}

int daDuskPuppet_c::Draw() {
    if (mpBody == NULL) {
        return 1;
    }
    // Same self-gate as Execute: don't draw during scene streaming / cutscenes
    // (the model's DL may have been freed by the archive-heap reset).
    if (fopOvlpM_IsDoingReq() != 0 || !daDuskPuppet_remotesVisible()) {
        return 1;
    }
    g_env_light.settingTevStruct(0x40, &current.pos, &tevStr);
    g_env_light.setLightTevColorType_MAJI(mpBody->getModel(), &tevStr);
    mpBody->entryDL();
    if (mpHead != NULL) {
        g_env_light.setLightTevColorType_MAJI(mpHead->getModel(), &tevStr);
        mpHead->entryDL();
    }
    if (mpFace != NULL) {
        g_env_light.setLightTevColorType_MAJI(mpFace->getModel(), &tevStr);
        mpFace->entryDL();
    }
    if (mpHands != NULL) {
        g_env_light.setLightTevColorType_MAJI(mpHands->getModel(), &tevStr);
        mpHands->entryDL();
    }
    static bool s_firstDrawn = false;
    if (!s_firstDrawn) { DuskLog.info("puppet: first frame fully drawn (CPU)"); s_firstDrawn = true; }
    return 1;
}

int daDuskPuppet_c::Delete() {
    // Nothing to release: our model copies live in the shared archive heap and
    // the McaMorf/J3DModel instances are freed with the actor's solid heap.
    return 1;
}

// ---- fpc method/profile registration ----

static int daDuskPuppet_Draw(daDuskPuppet_c* i_this)    { return i_this->Draw(); }
static int daDuskPuppet_Execute(daDuskPuppet_c* i_this) { return i_this->Execute(); }
static int daDuskPuppet_Delete(daDuskPuppet_c* i_this)  { return i_this->Delete(); }
static int daDuskPuppet_Create(fopAc_ac_c* i_this) {
    return static_cast<daDuskPuppet_c*>(i_this)->create();
}

static DUSK_CONST actor_method_class l_daDuskPuppet_Method = {
    (process_method_func)daDuskPuppet_Create,
    (process_method_func)daDuskPuppet_Delete,
    (process_method_func)daDuskPuppet_Execute,
    0,
    (process_method_func)daDuskPuppet_Draw,
};

DUSK_PROFILE actor_process_profile_definition DUSK_CONST g_profile_DUSK_PUPPET = {
    /* Layer ID     */ fpcLy_CURRENT_e,
    /* List ID      */ 3,
    /* List Prio    */ fpcPi_CURRENT_e,
    /* Proc Name    */ fpcNm_DUSK_PUPPET_e,
    /* Proc SubMtd  */ &g_fpcLf_Method.base,
    /* Size         */ sizeof(daDuskPuppet_c),
    /* Size Other   */ 0,
    /* Parameters   */ 0,
    /* Leaf SubMtd  */ &g_fopAc_Method.base,
    /* Draw Prio    */ fpcDwPi_Obj_TvCdlst_e,
    /* Actor SubMtd */ &l_daDuskPuppet_Method,
    /* Status       */ fopAcStts_UNK_0x40000_e | fopAcStts_UNK_0x4000_e,
    /* Group        */ fopAc_ACTOR_e,
    /* Cull Type    */ fopAc_CULLBOX_CUSTOM_e,
};

// ---- manager: one puppet per remote player ----

namespace {
// procID of the live puppet for each player id (0 = none).
fpc_ProcID s_puppets[256] = {};
// costume slot the live puppet was spawned with (to respawn on costume change).
u8 s_puppetSlot[256] = {};

void despawnAll() {
    for (int i = 0; i < 256; ++i) {
        if (s_puppets[i] != 0) {
            if (fopAcM_SearchByID(s_puppets[i]) != NULL) {
                fopAcM_delete(s_puppets[i]);
            }
            s_puppets[i] = 0;
        }
    }
}

}  // namespace

bool daDuskPuppet_remotesVisible() {
    if (!dusk::net::isConnected()) {
        return false;
    }
    daPy_py_c* link = daPy_getLinkPlayerActorClass();
    if (link == NULL) {
        return false;
    }
    // Never show remote players while a demo or cutscene is playing. The title
    // attract demo (Epona ride) runs the play scene with a live Link actor, so the
    // link!=NULL check above passes there — but entering a puppet model into that
    // demo scene's draw buffer corrupts its mat-packet list, so the Painter walks
    // it forever and the GX FIFO grows to ~2GB (realloc hang). Gating on demo mode
    // keeps remote players to real interactive gameplay only (and is the right
    // behaviour anyway: nothing should appear during the title/menu/cutscenes).
    if (link->getDemoMode() != 0) {
        return false;
    }
    // Also hide during scripted events / cutscenes: some run through the event
    // manager without setting the player's demo mode, and rendering a puppet into
    // a cutscene scene can crash (fragile draw buffer / scene streaming).
    if (dComIfGp_event_runCheck() != 0) {
        return false;
    }
    return true;
}

void daDuskPuppet_updateAll() {
    if (!daDuskPuppet_remotesVisible()) {
        despawnAll();
        return;
    }
    daPy_py_c* link = daPy_getLinkPlayerActorClass();

    // The engine is streaming a room/scene/cutscene: the archive heap is being
    // reset/repopulated, which FREES our cached costume models and anim BCK copies.
    // Drop every puppet + cache now so we never touch the dangling pointers (this
    // is the real cause of the cutscene crash — a remote triggering a cutscene
    // churns the heap under our cached anims), then rebuild fresh once loading is
    // done. The anm-archive-pointer change doesn't reliably fire here, so the
    // streaming flag is the signal we key off.
    if (fopOvlpM_IsDoingReq() != 0) {
        despawnAll();
        for (int i = 0; i < kCostumeCount; ++i) {
            s_costume[i] = CostumeModels();
        }
        s_animCacheCount = 0;
        s_scratch        = nullptr;
        s_animCacheArc   = nullptr;
        return;
    }

    const u32 localScene = dusk::net::getLocalSceneHash();
    bool present[256] = {};

    const int n = dusk::net::getRemotePlayerCount();
    for (int i = 0; i < n; ++i) {
        const dusk::net::PlayerState* p = dusk::net::getRemotePlayer(i);
        if (p == NULL || p->id == 0 || p->sceneHash != localScene) {
            continue;
        }
        // While a remote player is in a cutscene/demo they broadcast demo-archive
        // anim ids that aren't valid in our standard anm archive — loading them
        // produces a garbage BCK and crashes the puppet's skeleton calc. So while
        // that player is in a cutscene, keep their puppet DESPAWNED (their cutscene
        // plays locally with the real model; nothing to mirror). Leaving present[]
        // false makes the despawn pass below remove any existing puppet.
        if (p->flags & dusk::net::kFlagInCutscene) {
            continue;
        }
        present[p->id] = true;
        const int slot = costumeSlotFor(p->costume);

        // Drop a stale handle if the actor was culled/deleted (e.g. room change).
        if (s_puppets[p->id] != 0 && fopAcM_SearchByID(s_puppets[p->id]) == NULL) {
            s_puppets[p->id] = 0;
        }
        // Remote player changed clothes -> despawn so it respawns with the new model.
        if (s_puppets[p->id] != 0 && s_puppetSlot[p->id] != slot) {
            if (fopAcM_SearchByID(s_puppets[p->id]) != NULL) {
                fopAcM_delete(s_puppets[p->id]);
            }
            s_puppets[p->id] = 0;
        }
        if (s_puppets[p->id] != 0) {
            continue;  // already spawned with the right costume
        }

        // Mount/parse this costume's models before spawning. While it loads
        // (async) just try again next frame; don't block other players.
        if (!ensureCostumeReady(slot)) {
            continue;
        }

        cXyz pos(p->posX, p->posY, p->posZ);
        csXyz ang;
        ang.set(0, p->angleY, 0);

        // Spawn into the play scene's actor layer (same trick as the Actor Spawner).
        // Param packs the net id (low byte) + costume slot (next byte).
        layer_class* saved = fpcLy_CurrentLayer();
        base_process_class* play = fpcM_SearchByName(fpcNm_PLAY_SCENE_e);
        if (play != NULL) {
            fpcLy_SetCurrentLayer(&((process_node_class*)play)->layer);
        }
        fpc_ProcID id = fopAcM_create(fpcNm_DUSK_PUPPET_e,
                                      (u32)(p->id | (slot << 8)), &pos,
                                      link->current.roomNo, &ang, NULL, -1);
        fpcLy_SetCurrentLayer(saved);

        s_puppets[p->id]     = id;
        s_puppetSlot[p->id]  = (u8)slot;
    }

    // Despawn puppets whose player left this scene / disconnected.
    for (int id = 1; id < 256; ++id) {
        if (s_puppets[id] != 0 && !present[id]) {
            if (fopAcM_SearchByID(s_puppets[id]) != NULL) {
                fopAcM_delete(s_puppets[id]);
            }
            s_puppets[id] = 0;
        }
    }
}
