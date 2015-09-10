#include "args.h"
#include "bound_box.h"
#include "dataset.h"
#include "logger.h"
#include "verify.h"

#include "build_tree.h"
#include "kernel.h"
#include "traversal.h"

int main(int argc, char ** argv) {
  Args args(argc,argv);
  Bodies bodies, bodies2;
  BoundBox boundBox(args.nspawn);
  Bounds bounds;
  Dataset data;
  Verify verify;
  const int numBodies=args.numBodies;
  kernel::wavek = complex_t(10.,1.) / (2 * M_PI);
  logger::verbose = args.verbose;
  bodies = data.initBodies(args.numBodies, args.distribution, 0);
  bodies.resize(numBodies);
  logger::startTimer("Total FMM");
  logger::startTimer("Tree");
  bounds = boundBox.getBounds(bodies);
  Bodies buffer(numBodies);
  Cells cells = buildTree(bodies, buffer, bounds);
  logger::stopTimer("Tree");
  evaluate(cells);
  for (int i=0; i<numBodies; i++) {
    bodies[buffer[i].IBODY].TRG = buffer[i].TRG;
  }
  logger::stopTimer("Total FMM");
  const int numTarget = 100;
  bodies2.resize(numTarget);
  for (int i=0; i<numTarget; i++) {
    bodies2[i] = bodies[i];
    bodies2[i].TRG = 0;
  }
  cells.resize(2);
  C_iter Ci = cells.begin();
  C_iter Cj = cells.begin() + 1;
  Ci->BODY = bodies2.begin();
  Ci->NBODY = bodies2.size();
  Cj->BODY = bodies.begin();
  Cj->NBODY = bodies.size();
  logger::startTimer("Total Direct");
  real_t eps2 = 0;
  vec3 Xperiodic = 0;
  bool mutual = false;
  kernel::P2P(Ci, Cj, eps2, Xperiodic, mutual);
  logger::stopTimer("Total Direct");
  std::complex<double> potDif = verify.getDifScalar(bodies2, bodies);
  std::complex<double> potNrm = verify.getNrmScalar(bodies2);
  std::complex<double> accDif = verify.getDifVector(bodies2, bodies);
  std::complex<double> accNrm = verify.getNrmVector(bodies2);
  logger::printTitle("FMM vs. direct");
  verify.print("Rel. L2 Error (pot)",std::sqrt(potDif/potNrm));
  verify.print("Rel. L2 Error (acc)",std::sqrt(accDif/accNrm));
}
