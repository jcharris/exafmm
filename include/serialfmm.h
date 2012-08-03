#ifndef serialfmm_h
#define serialfmm_h
#include "treebuilder.h"

class SerialFMM : public TreeBuilder {
protected:
  real globalRadius;                                            //!< Radius of global root cell
  vec3 globalCenter;                                            //!< Center of global root cell
  vec3 globalXmin;                                              //!< Global Xmin for a given rank
  vec3 globalXmax;                                              //!< Global Xmax for a given rank

public:
//! Set center and size of root cell
  void setBounds(Bodies &bodies) {
    startTimer("Set bounds");                                   // Start timer
    localXmax = localXmin = bodies.begin()->X;                  // Initialize Xmin, Xmax
    for( B_iter B=bodies.begin(); B!=bodies.end(); ++B ) {      // Loop over bodies
      for( int d=0; d!=3; ++d ) {                               //  Loop over dimensions
        if     (B->X[d] < localXmin[d]) localXmin[d] = B->X[d]; //   Determine Xmin
        else if(B->X[d] > localXmax[d]) localXmax[d] = B->X[d]; //   Determine Xmax
      }                                                         //  End loop over dimensions
    }                                                           // End loop over bodies
    for( int d=0; d!=3; ++d ) {                                 // Loop over dimensions
      localCenter = (localXmax + localXmin) / 2;                //  Calculate center of domain
      localRadius = std::min(localCenter[d] - localXmin[d], localRadius);// Calculate min distance from center
      localRadius = std::max(localXmax[d] - localCenter[d], localRadius);// Calculate max distance from center 
    }                                                           // End loop over dimensions
    localRadius *= 1.000001;                                    // Add some leeway to radius
//    localRadius = M_PI;
//    localCenter = 0;
//    localXmin = -M_PI;
//    localXmax = M_PI;
    if( IMAGES == 0 ) {                                         // If non-periodic boundary condition
      globalRadius = localRadius;                               //  Set global radius for serial run
      globalCenter = localCenter;                               //  Set global center for serial run
      globalXmin = localXmin;                                   //  Set global Xmin for serial run
      globalXmax = localXmax;                                   //  Set global Xmax for serial run
    } else {                                                    // If periodic boundary condition
      globalRadius = M_PI;                                      //  Set global radius for serial run
      globalCenter = 0;                                         //  Set global radius for serial run
      globalXmin = -M_PI;                                       //  Set global Xmin for serial run
      globalXmax = M_PI;                                        //  Set global Xmax for serial run
    }                                                           // End if for periodic boundary condition
    stopTimer("Set bounds",printNow);                           // Stop timer
  }

//! Build tree structure top down
  void buildTree(Bodies &bodies, Cells &cells) {
    setLeafs(bodies);                                           // Copy bodies to leafs
    growTree();                                                 // Grow tree from root
    linkTree(bodies,cells);                                     // Form parent-child links in tree
  }

//! Upward pass (P2M, M2M)
  void upwardPass(Cells &cells) {
    startTimer("Upward pass");                                  // Start timer
    Ci0 = cells.begin();                                        // Set iterator of target root cell
    Cj0 = cells.begin();                                        // Set iterator of source root cell
    for( C_iter C=cells.end()-1; C!=cells.begin()-1; --C ) {    // Loop over cells bottomup
      real Rmax = 0;                                            //  Initialize Rmax
      setCenter(C);                                             //  Set center of cell to center of mass
      C->M = 0;                                                 //  Initialize multipole expansion coefficients
      C->L = 0;                                                 //  Initialize local expansion coefficients
      P2M(C,Rmax);                                              //  P2M kernel
      M2M(C,Rmax);                                              //  M2M kernel
    }                                                           // End loop over cells
    Ci0->X = localCenter;
    Ci0->R = localRadius;
#if Cartesian
    for( C_iter C=cells.begin(); C!=cells.end(); ++C ) {        // Loop over cells
      for( int i=1; i<MTERM; ++i ) C->M[i] /= C->M[0];          //  Normalize multipole expansion coefficients
    }                                                           // End loop over cells
#endif
    setRcrit(cells);                                            // Error optimization of Rcrit
    stopTimer("Upward pass",printNow);                          // Stop timer
  }

//! Interface for tree traversal when I = J (M2L, P2P)
  void evaluate(Cells &cells) {
    Ci0 = cells.begin();                                        // Set iterator of target root cell
    Cj0 = cells.begin();                                        // Set iterator of source root cell
    CellQueue cellQueue;                                        // Traversal queue
    pushCell(Ci0,cellQueue);                                    // Push root into queue
    Xperiodic = 0;                                              // No periodic shift
    startTimer("Traverse");                                     // Start timer
    traverse(cellQueue);                                        // Traverse the tree
    stopTimer("Traverse",printNow);                             // Stop timer
  }

//! Interface for tree traversal when I != J (M2L, P2P)
  void evaluate(Cells &icells, Cells &jcells) {
    Ci0 = icells.begin();                                       // Set iterator of target root cell
    Cj0 = jcells.begin();                                       // Set iterator of source root cell
    Pair pair(Ci0,Cj0);                                         // Pair of root cells
    PairQueue pairQueue;                                        // Traversal queue
    startTimer("Traverse");                                     // Start timer
    if( IMAGES == 0 ) {                                         // If non-periodic boundary condition
      Xperiodic = 0;                                            //  No periodic shift
      pairQueue.push_back(pair);                                //  Push root pair into queue
      traverse(pairQueue);                                      //  Traverse the tree
    } else {                                                    // If periodic boundary condition
      for( int ix=-1; ix<=1; ++ix ) {                           //  Loop over x periodic direction
        for( int iy=-1; iy<=1; ++iy ) {                         //   Loop over y periodic direction
          for( int iz=-1; iz<=1; ++iz ) {                       //    Loop over z periodic direction
            Xperiodic[0] = ix * 2 * globalRadius;               //     Coordinate shift for x periodic direction
            Xperiodic[1] = iy * 2 * globalRadius;               //     Coordinate shift for y periodic direction
            Xperiodic[2] = iz * 2 * globalRadius;               //     Coordinate shift for z periodic direction
            pairQueue.push_back(pair);                          //     Push pair to queue
            traverse(pairQueue);                                //     Traverse a pair of trees
          }                                                     //    End loop over z periodic direction
        }                                                       //   End loop over y periodic direction
      }                                                         //  End loop over x periodic direction
      traversePeriodic(globalRadius);                           //  Traverse tree for periodic images
    }                                                           // End if for periodic boundary condition
    stopTimer("Traverse",printNow);                             // Stop timer
  }

//! Downward pass (L2L, L2P)
  void downwardPass(Cells &cells) {
    startTimer("Downward pass");                                // Start timer
    C_iter C0 = cells.begin();                                  // Root cell
    L2P(C0);                                                    // If root is the only cell do L2P
    for( C_iter C=C0+1; C!=cells.end(); ++C ) {                 // Loop over cells
      L2L(C);                                                   //  L2L kernel
      L2P(C);                                                   //  L2P kernel
    }                                                           // End loop over cells
    stopTimer("Downward pass",printNow);                        // Stop timer
    if(printNow) printTreeData(cells);                          // Print tree data
  }

//! Direct summation
  void direct(Bodies &ibodies, Bodies &jbodies) {
    Cells cells(2);                                             // Define a pair of cells to pass to P2P kernel
    C_iter Ci = cells.begin(), Cj = cells.begin()+1;            // First cell is target, second cell is source
    Ci->LEAF = ibodies.begin();                                 // Iterator of first target leaf
    Ci->NDLEAF = ibodies.size();                                // Number of target leafs
    Cj->LEAF = jbodies.begin();                                 // Iterator of first source leaf
    Cj->NDLEAF = jbodies.size();                                // Number of source leafs
    int prange = 0;                                             // Range of periodic images
    for( int i=0; i<IMAGES; i++ ) {                             // Loop over periodic image sublevels
      prange += int(pow(3,i));                                  //  Accumulate range of periodic images
    }                                                           // End loop over perioidc image sublevels
    for( int ix=-prange; ix<=prange; ++ix ) {                   // Loop over x periodic direction
      for( int iy=-prange; iy<=prange; ++iy ) {                 //  Loop over y periodic direction
        for( int iz=-prange; iz<=prange; ++iz ) {               //   Loop over z periodic direction
          Xperiodic[0] = ix * 2 * globalRadius;                 //    Coordinate shift for x periodic direction
          Xperiodic[1] = iy * 2 * globalRadius;                 //    Coordinate shift for y periodic direction
          Xperiodic[2] = iz * 2 * globalRadius;                 //    Coordinate shift for z periodic direction
          P2P(Ci,Cj,false);                                     //    Evaluate P2P kernel
        }                                                       //   End loop over z periodic direction
      }                                                         //  End loop over y periodic direction
    }                                                           // End loop over x periodic direction
  }

//! Normalize bodies after direct summation
  void normalize(Bodies &bodies) {
    for( B_iter B=bodies.begin(); B!=bodies.end(); ++B ) {      // Loop over bodies
      B->TRG /= B->SRC;                                         //  Normalize by target charge
    }                                                           // End loop over bodies
  }
};

#endif