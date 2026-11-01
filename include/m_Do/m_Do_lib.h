#ifndef M_DO_M_DO_LIB_H
#define M_DO_M_DO_LIB_H

#include "JSystem/J3DU/J3DUClipper.h"

#if TARGET_PC
#include "helpers/gx_helper.h"
#endif

typedef struct Vec Vec;
struct ResTIMG;

#if DUSK_VR
namespace dusk::vr {
// While the game simulates, maps its camera's view to the headset's, or null.
MtxP sim_cull_correction();
}
#endif

struct mDoLib_clipper {
    static void setup(f32, f32, f32, f32);

    static void changeFar(f32 far_) {
        mClipper.setFar(far_);
        mClipper.calcViewFrustum();
    }

    static int clip(const Mtx m, const Vec* param_1, const Vec* param_2) {
#if DUSK_VR
        // In VR, cull against where the headset looks, not where the game's camera does.
        if (MtxP correction = dusk::vr::sim_cull_correction()) {
            Mtx headView;
            MTXConcat(correction, m, headView);
            return mClipper.clip(headView, (Vec*)param_1, (Vec*)param_2);
        }
#endif
        return mClipper.clip(m, (Vec*)param_1, (Vec*)param_2);
    }

    static s32 clip(const Mtx m, Vec param_1, f32 param_2) {
#if DUSK_VR
        if (MtxP correction = dusk::vr::sim_cull_correction()) {
            Mtx headView;
            MTXConcat(correction, m, headView);
            return mClipper.clip(headView, param_1, param_2);
        }
#endif
        return mClipper.clip(m, param_1, param_2);
    }

    static f32 getFar() { return mSystemFar; }
    static f32 getFovyRate() { return mFovyRate; }
    
    static void resetFar() {
        mClipper.setFar(mSystemFar);
        mClipper.calcViewFrustum();
    }

    static DUSK_GAME_DATA J3DUClipper mClipper;
    static DUSK_GAME_DATA f32 mSystemFar;
    static DUSK_GAME_DATA f32 mFovyRate;
};

void mDoLib_project(Vec* src, Vec* dst);
u32 mDoLib_setResTimgObj(ResTIMG const* res, TGXTexObj* o_texObj, u32 tlut_name,
                         TGXTlutObj* o_tlutObj);
void mDoLib_pos2camera(Vec* src, Vec* dst);

#if PLATFORM_WII
void mDoLib_2Dto3D(f32, f32, f32, Vec*);
#endif

#endif /* M_DO_M_DO_LIB_H */
