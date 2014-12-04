#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <omp.h>

typedef double real;
const int PP = 6;
const int MTERM = PP*(PP+1)*(PP+2)/6;
const int LTERM = (PP+1)*(PP+2)*(PP+3)/6;

#include "core.h"

#define for_3d for( int d=0; d<3; d++ )
#define for_4d for( int d=0; d<4; d++ )
#define for_m for( int m=0; m<MTERM; m++ )
#define for_l for( int l=0; l<LTERM; l++ )
#define FMMMAX(a,b) (((a) > (b)) ? (a) : (b))
#define FMMMIN(a,b) (((a) < (b)) ? (a) : (b))

#define NN 26
#define FN 37
#define CN 152

#define DIM 3

void executeStencilNN(int *nn_codes, int *node_code, int *stencil, int *periodic);
void executeStencilFN(int *fn_codes, int *node_codes, int *fn_stencil, int *cn_stencil, int *periodic);

class Kernel {
public:
  int maxLevel;
  int maxGlobLevel;
  int numBodies;
  int numImages;
  int numCells;
  int numLeafs;
  int numGlobCells;
  int numPartition[10][3];
  int globLevelOffset[10];
  int numSendBodies;
  int numSendCells;
  int numSendLeafs;
  int MPISIZE;
  int MPIRANK;

  real X0[3];
  real R0;
  real RGlob[3];
  int *Index;
  int *Index2;
  int *Rank;
  real (*Ibodies)[4];
  real (*Jbodies)[4];
  real (*Multipole)[MTERM];
  real (*Local)[LTERM];
  real (*globMultipole)[MTERM];
  real (*globLocal)[LTERM];
  int (*Leafs)[2];
  float (*sendJbodies)[4];
  float (*recvJbodies)[4];
  float (*sendMultipole)[MTERM];
  float (*recvMultipole)[MTERM];
  int (*sendLeafs)[2];
  int (*recvLeafs)[2];

private:
  inline void getIndex(int *ix, int index) const {
    for_3d ix[d] = 0;
    int d = 0, level = 0;
    while( index != 0 ) {
      ix[d] += (index % 2) * (1 << level);
      index >>= 1;
      d = (d+1) % 3;
      if( d == 0 ) level++;
    }
  }

  void getCenter(real *dist, int index, int level) const {
    real R = R0 / (1 << level);
    int ix[3] = {0, 0, 0};
    getIndex(ix, index);
    for_3d dist[d] = X0[d] - R0 + (2 * ix[d] + 1) * R;
  }

protected:
  inline int getGlobKey(int *ix, int level) const {
    return ix[0] + (ix[1] + ix[2] * numPartition[level][1]) * numPartition[level][0];
  }

  void P2P(int ibegin, int iend, int jbegin, int jend, real *periodic) const {
    int ii;
    for( ii=ibegin; ii<iend-1; ii+=2 ) {
#ifndef SPARC_SIMD
      for( int i=ii; i<=ii+1; i++ ) {
        real Po = 0, Fx = 0, Fy = 0, Fz = 0;
        for( int j=jbegin; j<jend; j++ ) {
          real dist[3];
          for_3d dist[d] = Jbodies[i][d] - Jbodies[j][d] - periodic[d];
          real R2 = dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2];
          real invR2 = 1.0 / R2;                                  
          if( R2 == 0 ) invR2 = 0;                                
          real invR = Jbodies[j][3] * sqrt(invR2);
          real invR3 = invR2 * invR;
          Po += invR;
          Fx += dist[0] * invR3;
          Fy += dist[1] * invR3;
          Fz += dist[2] * invR3;
        }
        Ibodies[i][0] += Po;
        Ibodies[i][1] -= Fx;
        Ibodies[i][2] -= Fy;
        Ibodies[i][3] -= Fz;
      }
#else
      __m128d Po = _mm_setzero_pd();
      __m128d Fx = _mm_setzero_pd();
      __m128d Fy = _mm_setzero_pd();
      __m128d Fz = _mm_setzero_pd();
      __m128d zero = _mm_setzero_pd();
      __m128d xi[3], xj[4];
      for_3d xi[d] = _mm_set_pd(Jbodies[ii][d]-periodic[d],Jbodies[ii+1][d]-periodic[d]);
      for( int j=jbegin; j<jend; j++ ) {
        for_4d xj[d] = _mm_set_pd(Jbodies[j][d],Jbodies[j][d]);
        for_3d xj[d] = _mm_sub_pd(xi[d],xj[d]);
        __m128d R2 = _mm_mul_pd(xj[0],xj[0]);
        R2 = _fjsp_madd_v2r8(xj[1],xj[1],R2);
        R2 = _fjsp_madd_v2r8(xj[2],xj[2],R2);
        __m128d invR = _fjsp_rsqrta_v2r8(R2);
        R2 = _mm_cmpneq_pd(R2,zero);
        invR = _mm_and_pd(invR,R2);
        R2 = _mm_mul_pd(invR,invR);
        invR = _mm_mul_pd(invR,xj[3]);
        R2 = _mm_mul_pd(R2,invR);
        Po = _mm_add_pd(Po,invR);
        Fx = _fjsp_madd_v2r8(xj[0],R2,Fx);
        Fy = _fjsp_madd_v2r8(xj[1],R2,Fy);
        Fz = _fjsp_madd_v2r8(xj[2],R2,Fz);
      }
      real Po2[2], Fx2[2], Fy2[2], Fz2[2];
      _mm_store_pd(Po2,Po);
      _mm_store_pd(Fx2,Fx);
      _mm_store_pd(Fy2,Fy);
      _mm_store_pd(Fz2,Fz);
      Ibodies[ii][0] += Po2[1];
      Ibodies[ii][1] -= Fx2[1];
      Ibodies[ii][2] -= Fy2[1];
      Ibodies[ii][3] -= Fz2[1];
      Ibodies[ii+1][0] += Po2[0];
      Ibodies[ii+1][1] -= Fx2[0];
      Ibodies[ii+1][2] -= Fy2[0];
      Ibodies[ii+1][3] -= Fz2[0];
#endif
    }
    for( int i=ii; i<iend; i++ ) {
      real Po = 0, Fx = 0, Fy = 0, Fz = 0;
      for( int j=jbegin; j<jend; j++ ) {
        real dist[3];
        for_3d dist[d] = Jbodies[i][d] - Jbodies[j][d] - periodic[d];
        real R2 = dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2];
        real invR2 = 1.0 / R2;
        if( R2 == 0 ) invR2 = 0;
        real invR = Jbodies[j][3] * sqrt(invR2);
        real invR3 = invR2 * invR;
        Po += invR;
        Fx += dist[0] * invR3;
        Fy += dist[1] * invR3;
        Fz += dist[2] * invR3;
      }
      Ibodies[i][0] += Po;
      Ibodies[i][1] -= Fx;
      Ibodies[i][2] -= Fy;
      Ibodies[i][3] -= Fz;
    }
  }

  void P2P() const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    int nunit = 1 << maxLevel;
    int nunitGlob[3];
    for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
    int nxmin[3], nxmax[3];
    for_3d nxmin[d] = -ixc[d] * nunit;
    for_3d nxmax[d] = nunitGlob[d] + nxmin[d] - 1;
    if( numImages != 0 ) {
      for_3d nxmin[d] -= nunitGlob[d];
      for_3d nxmax[d] += nunitGlob[d];
    }
    //#pragma omp parallel for
    for( int i=0; i<numLeafs; i++ ) {
      int ix[3] = {0, 0, 0};
      getIndex(ix,i);
      int jxmin[3];
      for_3d jxmin[d] = FMMMAX(nxmin[d],ix[d] - 1);
      int jxmax[3];
      for_3d jxmax[d] = FMMMIN(nxmax[d],ix[d] + 1) + 1;
      int jx[3];

      //printf("Particle to particle:\n");
      int nn_count = 0;
      //printf("%d, %d, %d:\n", ix[0], ix[1], ix[2]);
      for( jx[2]=jxmin[2]; jx[2]<jxmax[2]; jx[2]++ ) {
        for( jx[1]=jxmin[1]; jx[1]<jxmax[1]; jx[1]++ ) {
          for( jx[0]=jxmin[0]; jx[0]<jxmax[0]; jx[0]++ ) {
	    nn_count++;
            int jxp[3];
            for_3d jxp[d] = (jx[d] + nunit) % nunit;
            int j = getKey(jxp,maxLevel,false);
            for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
            int rankOffset = 13 * numLeafs;
#else
            int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif
	    /*
	    if(ix[0]==3 && ix[1]==3 && ix[2]==3){
	      printf("jx: %d, %d, %d\n", jx[0], jx[1], jx[2]);
	    }
	    */

	    j += rankOffset;
            rankOffset = 13 * numLeafs;
            real periodic[3] = {0, 0, 0};
            for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
            for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
            P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);
          }
        }
      }
      //printf("nn: %d\n", nn_count);
    }
  }


  void P2P(int *nn_stencil) const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    int nunit = 1 << maxLevel;
    int nunitGlob[3];
    for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
    int nxmin[3], nxmax[3];
    for_3d nxmin[d] = -ixc[d] * nunit;
    for_3d nxmax[d] = nunitGlob[d] + nxmin[d] - 1;
    if( numImages != 0 ) {
      for_3d nxmin[d] -= nunitGlob[d];
      for_3d nxmax[d] += nunitGlob[d];
    }
#pragma omp parallel for                                                                                                                                           
    for( int i=0; i<numLeafs; i++ ) {
      int ix[3] = {0, 0, 0};
      getIndex(ix,i);
      int jxmin[3];
      for_3d jxmin[d] = FMMMAX(nxmin[d],ix[d] - 1);
      int jxmax[3];
      for_3d jxmax[d] = FMMMIN(nxmax[d],ix[d] + 1) + 1;
      int jx[3];

      int px[3];
      px[0] = 2 * (ix[0] / 2);
      px[1] = 2 * (ix[1] / 2);
      px[2] = 2 * (ix[2] / 2);

      //int loc = (ix[0] & 1) | ( (ix[1] & 1) << 1 ) | ( (ix[2] & 1) << 2 ); 
      int loc = ( (ix[0] & 1) << 2 ) | ( (ix[1] & 1) << 1 ) | ( (ix[2] & 1) );
      //printf("Particle to particle:\n");                                                                                                                              

      int nn_count = 0;
      //printf("%d\n", loc);
      //printf("min: %d, %d, %d\n", nxmin[0], nxmin[1], nxmin[2]);
      //printf("max: %d, %d, %d\n", nxmax[0], nxmax[1], nxmax[2]);          

#if 1

      for(int k=0; k<NN; k++){
	jx[2] = px[2] + nn_stencil[loc*DIM*NN + k*DIM + 0];
	//if(jx[2] >= nxmin[2] && jx[2] <= nxmax[0]){
	  jx[1] = px[1] + nn_stencil[loc*DIM*NN + k*DIM + 1];
	  //if(jx[1] >= nxmin[1] && jx[1] <= nxmax[1]){
	    jx[0] = px[0] + nn_stencil[loc*DIM*NN + k*DIM + 2];
	    //if(jx[0] >= nxmin[0] && jx[0] <= nxmax[0]){
	      nn_count++;
	      int jxp[3];
	      for_3d jxp[d] = (jx[d] + nunit) % nunit;
	      int j = getKey(jxp,maxLevel,false);
	      for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
	      int rankOffset = 13 * numLeafs;
#else
	      int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif
	      /*
	      if(ix[0]==1 && ix[1]==3 && ix[2]==0){
		printf("%d: %d, jx: %d, %d, %d\n", nn_count, loc, jx[0], jx[1], jx[2]);
		printf("index: %d\n", k);
	       }
	      */

	      j += rankOffset;
	      rankOffset = 13 * numLeafs;
	      real periodic[3] = {0, 0, 0};
	      for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
	      for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
	      P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);
	      //}
	      //}
	      //}
      }
      /* Particle to Particle interactions in the same bin */
      int jxp[3];
      jx[0] = ix[0]; jx[1] = ix[1]; jx[2] = ix[2];
      for_3d jxp[d] = (jx[d] + nunit) % nunit;
      int j = getKey(jxp,maxLevel,false);
      for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
      int rankOffset = 13 * numLeafs;
#else
      int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif

      j += rankOffset;
      rankOffset = 13 * numLeafs;
      real periodic[3] = {0, 0, 0};
      for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
      for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
      P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);
#endif

#if 0
      //printf("%d\n", loc);
      nn_count = 0;
      for( jx[2]=jxmin[2]; jx[2]<jxmax[2]; jx[2]++ ) {
        for( jx[1]=jxmin[1]; jx[1]<jxmax[1]; jx[1]++ ) {
          for( jx[0]=jxmin[0]; jx[0]<jxmax[0]; jx[0]++ ) {
            nn_count++;
            int jxp[3];
            for_3d jxp[d] = (jx[d] + nunit) % nunit;
            int j = getKey(jxp,maxLevel,false);
            for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
            int rankOffset = 13 * numLeafs;
#else
            int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif
	    /*	    
            if(ix[0]==1 && ix[1]==3 && ix[2]==0){
              printf("%d, loc: %d, jx: %d, %d, %d\n", nn_count, loc, jx[0], jx[1], jx[2]);
	    }
	    */

            j += rankOffset;
            rankOffset = 13 * numLeafs;
            real periodic[3] = {0, 0, 0};
            for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
            for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
            P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);
          }
        }
      }
#endif
      //printf("nn: %d\n", nn_count);                                                                                                                                   
    }
  }


  void P2P_stencil(int *nn_stencil) const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    int nunit = 1 << maxLevel;
    int nunitGlob[3];
    for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
    int nxmin[3], nxmax[3];
    for_3d nxmin[d] = -ixc[d] * nunit;
    for_3d nxmax[d] = nunitGlob[d] + nxmin[d] - 1;
    if( numImages != 0 ) {
      for_3d nxmin[d] -= nunitGlob[d];
      for_3d nxmax[d] += nunitGlob[d];
    }
#pragma omp parallel for
    for( int i=0; i<numLeafs; i++ ) {
      int ix[3] = {0, 0, 0};
      getIndex(ix,i);
      int jxmin[3];
      for_3d jxmin[d] = FMMMAX(nxmin[d],ix[d] - 1);
      int jxmax[3];
      for_3d jxmax[d] = FMMMIN(nxmax[d],ix[d] + 1) + 1;
      int jx[3];

      int node_code[DIM];
      node_code[0] = ix[2]; node_code[1] = ix[1]; node_code[2] = ix[0];

      int nn_codes[DIM*NN];
      executeStencilNN(nn_codes, node_code, nn_stencil, 0);

#if 1

      for(int k=0; k<NN; k++){
        jx[2] = nn_codes[k*DIM + 0];
	jx[1] = nn_codes[k*DIM + 1];
	jx[0] = nn_codes[k*DIM + 2];
	int jxp[3];
	for_3d jxp[d] = (jx[d] + nunit) % nunit;
	int j = getKey(jxp,maxLevel,false);
	for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
	int rankOffset = 13 * numLeafs;
#else
	int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif

	j += rankOffset;
	rankOffset = 13 * numLeafs;
	real periodic[3] = {0, 0, 0};
	for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
	for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
	P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);

      }
      /* Particle to Particle interactions in the same bin */
      int jxp[3];
      jx[0] = ix[0]; jx[1] = ix[1]; jx[2] = ix[2];
      for_3d jxp[d] = (jx[d] + nunit) % nunit;
      int j = getKey(jxp,maxLevel,false);
      for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
      int rankOffset = 13 * numLeafs;
#else
      int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numLeafs;
#endif

      j += rankOffset;
      rankOffset = 13 * numLeafs;
      real periodic[3] = {0, 0, 0};
      for_3d jxp[d] = (jx[d] + ixc[d] * nunit + nunitGlob[d]) / nunitGlob[d];
      for_3d periodic[d] = (jxp[d] - 1) * 2 * RGlob[d];
      P2P(Leafs[i+rankOffset][0],Leafs[i+rankOffset][1],Leafs[j][0],Leafs[j][1],periodic);
#endif
    }
  }


  void P2M() const {
    int rankOffset = 13 * numLeafs;
    int levelOffset = ((1 << 3 * maxLevel) - 1) / 7 + 13 * numCells;
#pragma omp parallel for
    for( int i=0; i<numLeafs; i++ ) {
      real center[3];
      getCenter(center,i,maxLevel);
      for( int j=Leafs[i+rankOffset][0]; j<Leafs[i+rankOffset][1]; j++ ) {
        real dist[3];
        for_3d dist[d] = center[d] - Jbodies[j][d];
        real M[MTERM];
        M[0] = Jbodies[j][3];
        powerM(M,dist);
        for_m Multipole[i+levelOffset][m] += M[m];
      }
    }
  }

  void M2M() const {
    int rankOffset = 13 * numCells;
    for( int lev=maxLevel; lev>0; lev-- ) {
      int childOffset = ((1 << 3 * lev) - 1) / 7 + rankOffset;
      int parentOffset = ((1 << 3 * (lev - 1)) - 1) / 7 + rankOffset;
      real radius = R0 / (1 << lev);
#pragma omp parallel for schedule(static, 8)
      for( int i=0; i<(1 << 3 * lev); i++ ) {
        int c = i + childOffset;
        int p = (i >> 3) + parentOffset;
        int ix[3];
        ix[0] = 1 - (i & 1) * 2;
        ix[1] = 1 - ((i / 2) & 1) * 2;
        ix[2] = 1 - ((i / 4) & 1) * 2;
        real dist[3];
        for_3d dist[d] = ix[d] * radius;
        real M[MTERM];
        real C[LTERM];
        C[0] = 1;
        powerM(C,dist);
        for_m M[m] = Multipole[c][m];
        for_m Multipole[p][m] += C[m] * M[0];
        M2MSum(Multipole[p],C,M);
      }
    }
  }

  void M2L() const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    for( int lev=1; lev<=maxLevel; lev++ ) {
      int levelOffset = ((1 << 3 * lev) - 1) / 7;
      int nunit = 1 << lev;
      int nunitGlob[3];
      for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
      int nxmin[3], nxmax[3];
      for_3d nxmin[d] = -ixc[d] * (nunit >> 1);
      for_3d nxmax[d] = (nunitGlob[d] >> 1) + nxmin[d] - 1;
      if( numImages != 0 ) {
        for_3d nxmin[d] -= (nunitGlob[d] >> 1);
        for_3d nxmax[d] += (nunitGlob[d] >> 1);
      }
      real diameter = 2 * R0 / (1 << lev);
#pragma omp parallel for
      for( int i=0; i<(1 << 3 * lev); i++ ) {
        real L[LTERM];
        for_l L[l] = 0;
        int ix[3] = {0, 0, 0};
        getIndex(ix,i);
        int jxmin[3];
        for_3d jxmin[d] =  FMMMAX(nxmin[d],(ix[d] >> 1) - 1)      << 1;
        int jxmax[3];
        for_3d jxmax[d] = (FMMMIN(nxmax[d],(ix[d] >> 1) + 1) + 1) << 1;
        int jx[3];
        for( jx[2]=jxmin[2]; jx[2]<jxmax[2]; jx[2]++ ) {
          for( jx[1]=jxmin[1]; jx[1]<jxmax[1]; jx[1]++ ) {
            for( jx[0]=jxmin[0]; jx[0]<jxmax[0]; jx[0]++ ) {
              if(jx[0] < ix[0]-1 || ix[0]+1 < jx[0] ||
                 jx[1] < ix[1]-1 || ix[1]+1 < jx[1] ||
                 jx[2] < ix[2]-1 || ix[2]+1 < jx[2]) {
                int jxp[3];
                for_3d jxp[d] = (jx[d] + nunit) % nunit;
                int j = getKey(jxp,lev);
                for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
                int rankOffset = 13 * numCells;
#else
                int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numCells;
#endif
                j += rankOffset;
                real M[MTERM];
                for_m M[m] = Multipole[j][m];
                real dist[3];
                for_3d dist[d] = (ix[d] - jx[d]) * diameter;
                real invR2 = 1. / (dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
                real invR  = sqrt(invR2);
                real C[LTERM];
                getCoef(C,dist,invR2,invR);
                M2LSum(L,C,M);
              }
            }
          }
        }
        for_l Local[i+levelOffset][l] += L[l];
      }
    }
  }

  void M2L(int *common_stencil, int *far_stencil) const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    for( int lev=1; lev<=maxLevel; lev++ ) {
      int levelOffset = ((1 << 3 * lev) - 1) / 7;
      int nunit = 1 << lev;
      int nunitGlob[3];
      for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
      int nxmin[3], nxmax[3];
      for_3d nxmin[d] = -ixc[d] * (nunit >> 1);
      for_3d nxmax[d] = (nunitGlob[d] >> 1) + nxmin[d] - 1;
      if( numImages != 0 ) {
        for_3d nxmin[d] -= (nunitGlob[d] >> 1);
        for_3d nxmax[d] += (nunitGlob[d] >> 1);
      }
      real diameter = 2 * R0 / (1 << lev);

      //printf("l%d max: %d, %d, %d\n", lev, nxmax[0], nxmax[1], nxmax[2]);
      //printf("l%d min: %d, %d, %d\n", lev, nxmin[0], nxmin[1], nxmin[2]);

#pragma omp parallel for
      for( int i=0; i<(1 << 3 * lev); i++ ) {
        real L[LTERM];
        for_l L[l] = 0;
        int ix[3] = {0, 0, 0};
        getIndex(ix,i);
        int jxmin[3];
        for_3d jxmin[d] =  FMMMAX(nxmin[d],(ix[d] >> 1) - 1)      << 1;
        int jxmax[3];
        for_3d jxmax[d] = (FMMMIN(nxmax[d],(ix[d] >> 1) + 1) + 1) << 1;
        int jx[3];

#if 1
	int px[3];
	int loc = ( (ix[0] & 1) << 2 ) | ( (ix[1] & 1) << 1 ) | ( (ix[2] & 1) );

	px[0] = 2 * (ix[0] / 2);
	px[1] = 2 * (ix[1] / 2);
	px[2] = 2 * (ix[2] / 2);

#if 1
	/* loop over the common neighbors */
	for(int k=0; k<CN; k++){
	  jx[2] = px[2] + common_stencil[k*DIM + 0]; 
	  //if(jx[2] >= (nxmin[2]-2) && jx[2] <= (nxmax[2]+2)){
	    jx[1] = px[1] + common_stencil[k*DIM + 1];
	    //if(jx[1] >= (nxmin[1]-2) && jx[1] <= (nxmax[1]+2)){
	      jx[0] = px[0] + common_stencil[k*DIM + 2];
	      //if(jx[0] >= (nxmin[0]-2) && jx[0] <= (nxmax[0]+2)){
                int jxp[3];
                for_3d jxp[d] = (jx[d] + nunit) % nunit;
                int j = getKey(jxp,lev);
                for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
                int rankOffset = 13 * numCells;
#else
                int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numCells;
#endif
		/*
                if(ix[0]==1 && ix[1]==1 && ix[2]==1 && lev==1){
                  printf("%d, %d, %d\n", jx[0], jx[1], jx[2]);
                }
		*/

                j += rankOffset;
                real M[MTERM];
                for_m M[m] = Multipole[j][m];
                real dist[3];
                for_3d dist[d] = (ix[d] - jx[d]) * diameter;
                real invR2 = 1. / (dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
                real invR  = sqrt(invR2);
                real C[LTERM];
                getCoef(C,dist,invR2,invR);
                M2LSum(L,C,M);
		//}
		//}
		//}
	}
#endif

#if 1
	/* loop over the private neighbors */
	for(int k=0; k<FN; k++){
	  jx[2] = px[2] + far_stencil[loc*DIM*FN + k*DIM + 0];
	  //if(jx[2] >= (nxmin[2]-2) && jx[2] <= (nxmax[2]+2)){
	    jx[1] = px[1] + far_stencil[loc*DIM*FN + k*DIM + 1];
	    //if(jx[1] >= (nxmin[1]-2) && jx[1] <= (nxmax[1]+2)){
	      jx[0] = px[0] + far_stencil[loc*DIM*FN + k*DIM + 2];
	      //if(jx[0] >= (nxmin[0]-2) && jx[0] <= (nxmax[0]+2)){
                int jxp[3];
                for_3d jxp[d] = (jx[d] + nunit) % nunit;
                int j = getKey(jxp,lev);
                for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
                int rankOffset = 13 * numCells;
#else
                int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numCells;
#endif
		/*
		if(ix[0]==1 && ix[1]==1 && ix[2]==1 && lev==1){
		  printf("lc:%d: %d, %d, %d\n", loc, jx[0], jx[1], jx[2]);
		}
		*/		

                j += rankOffset;
                real M[MTERM];
                for_m M[m] = Multipole[j][m];
                real dist[3];
                for_3d dist[d] = (ix[d] - jx[d]) * diameter;
                real invR2 = 1. / (dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
                real invR  = sqrt(invR2);
                real C[LTERM];
                getCoef(C,dist,invR2,invR);
                M2LSum(L,C,M);
		//}
		//}
		//}
	}
#endif
#endif

#if 0

	int fn_count = 0;
        for( jx[2]=jxmin[2]; jx[2]<jxmax[2]; jx[2]++ ) {
          for( jx[1]=jxmin[1]; jx[1]<jxmax[1]; jx[1]++ ) {
            for( jx[0]=jxmin[0]; jx[0]<jxmax[0]; jx[0]++ ) {
              if(jx[0] < ix[0]-1 || ix[0]+1 < jx[0] ||
                 jx[1] < ix[1]-1 || ix[1]+1 < jx[1] ||
                 jx[2] < ix[2]-1 || ix[2]+1 < jx[2]) {
		fn_count++;
                int jxp[3];
                for_3d jxp[d] = (jx[d] + nunit) % nunit;
                int j = getKey(jxp,lev);
                for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
                int rankOffset = 13 * numCells;
#else
                int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numCells;
#endif
		/*		
                if(ix[0]==1 && ix[1]==1 && ix[2]==1 && lev==1){
                  printf("%d: %d, %d, %d\n", fn_count, jx[0], jx[1], jx[2]);
                }
		*/

                j += rankOffset;
                real M[MTERM];
                for_m M[m] = Multipole[j][m];
                real dist[3];
                for_3d dist[d] = (ix[d] - jx[d]) * diameter;
                real invR2 = 1. / (dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
                real invR  = sqrt(invR2);
                real C[LTERM];
                getCoef(C,dist,invR2,invR);
                M2LSum(L,C,M);
              }
            }
          }
        }
	//printf("fn:%d of %d\n", fn_count, CN+FN);
#endif
        for_l Local[i+levelOffset][l] += L[l];
      }
    }
  }

  void M2L_stencil(int *common_stencil, int *far_stencil) const {
    int ixc[3];
    getGlobIndex(ixc,MPIRANK,maxGlobLevel);
    for( int lev=1; lev<=maxLevel; lev++ ) {
      int levelOffset = ((1 << 3 * lev) - 1) / 7;
      int nunit = 1 << lev;
      int nunitGlob[3];
      for_3d nunitGlob[d] = nunit * numPartition[maxGlobLevel][d];
      int nxmin[3], nxmax[3];
      for_3d nxmin[d] = -ixc[d] * (nunit >> 1);
      for_3d nxmax[d] = (nunitGlob[d] >> 1) + nxmin[d] - 1;
      if( numImages != 0 ) {
        for_3d nxmin[d] -= (nunitGlob[d] >> 1);
        for_3d nxmax[d] += (nunitGlob[d] >> 1);
      }
      real diameter = 2 * R0 / (1 << lev);

#pragma omp parallel for
      for( int i=0; i<(1 << 3 * lev); i++ ) {
        real L[LTERM];
        for_l L[l] = 0;
        int ix[3] = {0, 0, 0};
        getIndex(ix,i);
        int jxmin[3];
        for_3d jxmin[d] =  FMMMAX(nxmin[d],(ix[d] >> 1) - 1)      << 1;
        int jxmax[3];
        for_3d jxmax[d] = (FMMMIN(nxmax[d],(ix[d] >> 1) + 1) + 1) << 1;
        int jx[3];

	int node_code[DIM];
	node_code[0] = ix[2]; node_code[1] = ix[1]; node_code[2] = ix[0];

	int fn_codes[DIM*(FN+CN)];

	executeStencilFN(fn_codes, node_code, far_stencil, common_stencil, 0);

        /* loop over the common neighbors */
        for(int k=0; k<(CN+FN); k++){
          jx[2] = fn_codes[k*DIM + 0];
	  jx[1] = fn_codes[k*DIM + 1];
	  jx[0] = fn_codes[k*DIM + 2];

	  int jxp[3];
	  for_3d jxp[d] = (jx[d] + nunit) % nunit;
	  int j = getKey(jxp,lev);
	  for_3d jxp[d] = (jx[d] + nunit) / nunit;
#if Serial
	  int rankOffset = 13 * numCells;
#else
	  int rankOffset = (jxp[0] + 3 * jxp[1] + 9 * jxp[2]) * numCells;
#endif

	  j += rankOffset;
	  real M[MTERM];
	  for_m M[m] = Multipole[j][m];
	  real dist[3];
	  for_3d dist[d] = (ix[d] - jx[d]) * diameter;
	  real invR2 = 1. / (dist[0] * dist[0] + dist[1] * dist[1] + dist[2] * dist[2]);
	  real invR  = sqrt(invR2);
	  real C[LTERM];
	  getCoef(C,dist,invR2,invR);
	  M2LSum(L,C,M);
        }
        for_l Local[i+levelOffset][l] += L[l];
      }
    }
  }


  void L2L() const {
    for( int lev=1; lev<=maxLevel; lev++ ) {
      int childOffset = ((1 << 3 * lev) - 1) / 7;
      int parentOffset = ((1 << 3 * (lev - 1)) - 1) / 7;
      real radius = R0 / (1 << lev);
#pragma omp parallel for
      for( int i=0; i<(1 << 3 * lev); i++ ) {
        int c = i + childOffset;
        int p = (i >> 3) + parentOffset;
        int ix[3];
        ix[0] = (i & 1) * 2 - 1;
        ix[1] = ((i / 2) & 1) * 2 - 1;
        ix[2] = ((i / 4) & 1) * 2 - 1;
        real dist[3];
        for_3d dist[d] = ix[d] * radius;
        real C[LTERM];
        C[0] = 1;
        powerL(C,dist);
        for_l Local[c][l] += Local[p][l];
        for( int l=1; l<LTERM; l++ ) Local[c][0] += C[l] * Local[p][l];
        L2LSum(Local[c],C,Local[p]);
      }
    }
  }

  void L2P() const {
    int rankOffset = 13 * numLeafs;
    int levelOffset = ((1 << 3 * maxLevel) - 1) / 7;
#pragma omp parallel for
    for( int i=0; i<numLeafs; i++ ) {
      real center[3];
      getCenter(center,i,maxLevel);
      real L[LTERM];
      for_l L[l] = Local[i+levelOffset][l];
      for( int j=Leafs[i+rankOffset][0]; j<Leafs[i+rankOffset][1]; j++ ) {
        real dist[3];
        for_3d dist[d] = Jbodies[j][d] - center[d];
        real C[LTERM];
        C[0] = 1;
        powerL(C,dist);
        for_4d Ibodies[j][d] += L[d];
        for( int l=1; l<LTERM; l++ ) Ibodies[j][0] += C[l] * L[l];
        L2PSum(Ibodies[j],C,L);
      }
    }
  }

public:
  Kernel() : MPISIZE(1), MPIRANK(0) {}
  ~Kernel() {}

  inline int getKey(int *ix, int level, bool levelOffset=true) const {
    int id = 0;
    if( levelOffset ) id = ((1 << 3 * level) - 1) / 7;
    for( int lev=0; lev<level; ++lev ) {
      for_3d id += ix[d] % 2 << (3 * lev + d);
      for_3d ix[d] >>= 1;
    }
    return id;
  }

  inline void getGlobIndex(int *ix, int index, int level) const {
    ix[0] = index % numPartition[level][0];
    ix[1] = index / numPartition[level][0] % numPartition[level][1];
    ix[2] = index / numPartition[level][0] / numPartition[level][1];
  }
};
