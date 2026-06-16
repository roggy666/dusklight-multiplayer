#ifndef D_A_DUSK_PUPPET_H
#define D_A_DUSK_PUPPET_H

#include "f_op/f_op_actor_mng.h"
#include "d/d_com_inf_game.h"
#include "JSystem/J3DGraphBase/J3DTransform.h"  // J3DTransformInfo (per-joint pose cache)

class mDoExt_McaMorf;
class J3DAnmTransform;

// Dusklight multiplayer remote-player puppet (Path B).
//
// A passive actor that renders the human Link body model ("Kmdl"/"al.bmd") at a
// remote player's transform. One puppet per remote player; lifecycle is owned by
// daDuskPuppet_updateAll() (the manager). This is NOT the player actor (Link is a
// singleton via dComIfGp_setPlayer); it just draws a Link-shaped body.

class daDuskPuppet_c : public fopAc_ac_c {
public:
    enum { kMaxJoints = 64 };  // Link body has ~0x1B joints; mPose array is guarded

    int create();
    int CreateHeap();
    static int createHeapCallBack(fopAc_ac_c*);
    void setBaseMtx();
    int Execute();
    int Draw();
    int Delete();

private:
    // Link's human model is split across several BMDs attached to body joints.
    mDoExt_McaMorf* mpBody;    // al.bmd
    mDoExt_McaMorf* mpHead;    // al_head.bmd  (hair/head; body joint 4)
    mDoExt_McaMorf* mpFace;    // al_face.bmd  (eyes/mouth; body joint 4)
    mDoExt_McaMorf* mpHands;   // al_hands.bmd (joints 9 / 0xE)
    u8              mRemoteId;  // which net player this puppet mirrors
    u8              mSlot;      // costume slot (which model set: casual/hero/zora/magic)
    bool            mAnimStarted;  // body model created / anim path live
    u16             mCurAnimId;    // lower-body (legs) BCK id currently playing
    u16             mCurUpperId;   // upper-body (arms) BCK id currently playing
    float           mUnderFrame;   // playback frame of the lower-body anim
    float           mUpperFrame;   // playback frame of the upper-body anim
    // Animation crossfade. When a layer's anim id changes we blend the persistent
    // per-joint pose toward the NEW anim's live pose over kMorphFrames, mirroring
    // the engine's incremental morf (mDoExt_McaMorf::calc): each frame nudges the
    // stored pose a fraction of the remaining gap toward the moving target — NOT a
    // frozen prev->cur lerp (that hand-rolled version lagged the arms). Rotation is
    // shortest-arc s16. Lower (legs) and upper (arms) layers crossfade
    // independently so e.g. drawing a sword doesn't restart the legs.
    bool             mMorphReady;  // one full body calc done -> safe to crossfade
    float            mUnderMorf;   // lower-layer morf progress 0..1 (1 = settled)
    float            mUpperMorf;   // upper-layer morf progress 0..1
    bool             mUnderMorfing;// lower layer mid-crossfade
    bool             mUpperMorfing;// upper layer mid-crossfade
    J3DTransformInfo mPose[kMaxJoints];  // persistent blended pose per body joint
};

// Spawn / update / despawn one puppet per remote player in the local scene.
// Call once per frame (PC build, while a game is running).
void daDuskPuppet_updateAll();

// True when remote players should be shown (connected, local Link exists, and no
// demo/cutscene is playing). Both the puppet manager and the in-world nameplates
// gate on this so neither appears during the title attract demo / cutscenes.
bool daDuskPuppet_remotesVisible();

#endif /* D_A_DUSK_PUPPET_H */
