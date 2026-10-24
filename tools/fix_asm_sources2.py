import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import sub, done

S = 'src/JSystem/J3DGraphBase/J3DShapeMtx.cpp'
sub(S, '''        psq_st fr1, 0x18(destination), 0, 0
        stfs fr0, 0x20(destination)
    }
#endif
}''', '''        psq_st fr1, 0x18(destination), 0, 0
        stfs fr0, 0x20(destination)
    }
#else
    __builtin_memcpy(dst, src, sizeof(Mtx33));
#endif
}''')

sub(S, '''        psq_st u, 24(dst), 0, 0
        stfs v, 32(dst)
    }
#endif
}''', '''        psq_st u, 24(dst), 0, 0
        stfs v, 32(dst)
    }
#else
    for (int r = 0; r < 3; r++) {
        dst[r][0] = src[r][0];
        dst[r][1] = src[r][1];
        dst[r][2] = src[r][2];
    }
#endif
}''')

B = 'src/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp'
sub(B, '''    __REGISTER f32* var_r7 = J3DUnit01;

    i = -1;''', '''    __REGISTER f32* var_r7 = J3DUnit01;

#ifndef __MWERKS__
    // weightAnmMtx[e] = sum over the envelope's joints of weight * (anmMtx[joint] * invJointMtx[joint]).
    {
        const u16* pIdx = mJointTree->getWEvlpMixMtxIndex();
        const f32* pWeight = mJointTree->getWEvlpMixWeight();
        int envNum = mJointTree->getWEvlpMtxNum();
        for (int e = 0; e < envNum; e++) {
            u8* pEnvScale = &mpEvlpScaleFlagArr[e];
            *pEnvScale = 1;
            Mtx acc;
            for (int r = 0; r < 3; r++) {
                acc[r][0] = acc[r][1] = acc[r][2] = acc[r][3] = 0.0f;
            }
            int envMixNum = mJointTree->getWEvlpMixMtxNum(e);
            int k = 0;
            do {
                u16 jointIdx = *pIdx++;
                f32 w = *pWeight++;
                Mtx prod;
                PSMTXConcat(mpAnmMtx[jointIdx], mJointTree->getInvJointMtx(jointIdx), prod);
                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 4; c++) {
                        acc[r][c] += prod[r][c] * w;
                    }
                }
                *pEnvScale &= mpScaleFlagArr[jointIdx];
            } while (++k < envMixNum);
            PSMTXCopy(acc, mpWeightEvlpMtx[e]);
        }
    }
#else

    i = -1;''')

sub(B, '''            psq_st var_f30, 40(weightAnmMtx), 0, 0
            ps_merge00 var_f30, var_f24, var_f24
        }
    }
}''', '''            psq_st var_f30, 40(weightAnmMtx), 0, 0
            ps_merge00 var_f30, var_f24, var_f24
        }
    }
#endif
}''')

done()
