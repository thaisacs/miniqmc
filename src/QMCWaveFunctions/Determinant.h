////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source
// License.  See LICENSE file in top directory for details.
//
// Copyright (c) 2017 QMCPACK developers.
//
// File developed by: M. Graham Lopez
//
// File created by: M. Graham Lopez
////////////////////////////////////////////////////////////////////////////////
// -*- C++ -*-

/**
 * @file Determinant.h
 * @brief Determinant piece of the wave function
 */

// need to implement: multi_evaluateLog, multi_evaluateGL, multi_evalGrad,
//                    multi_ratioGrad, multi_acceptrestoreMove (under the hood calls acceptMove)

// strategy is to make a View of DiracDeterminantKokkos and then to write functions
// that contain kernels that work on them.  This will allow us to follow the pointers
// during the parallel evaluation and potentially do more parallel work

#ifndef QMCPLUSPLUS_DETERMINANT_H
#define QMCPLUSPLUS_DETERMINANT_H

#include <Kokkos_Core.hpp>
#include <impl/Kokkos_Timer.hpp>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#ifdef KOKKOS_ENABLE_CUDA
#include "cublas_v2.h"
#include "cusolverDn.h"
#endif

#include "QMCWaveFunctions/WaveFunctionComponent.h"
#include "Numerics/LinAlgKokkos.h"
//#include "Utilities/RandomGenerator.h"


namespace qmcplusplus
{


template<class linAlgHelperType, typename ValueType>

ValueType InvertWithLog(DiracDeterminantKokkos& ddk, LinAlgHelperType& lah, ValueType& phase) {
  ValueType locLogDet(0.0);
  lah.getrf(ddk.psiM);
  auto locPiv = lah.getPivot(); // note, this is in device memory
  int sign_det = 1;

  Kokkos::parallel_reduce(view.extent(0), KOKKOS_LAMBDA ( int ii, int& cur_sign) {
      cur_sign = (locPiv(i) == i+1) ? 1 : -1;
      cur_sign *= (ddk.psiM(i,i) > 0) ? 1 : -1;
    }, Kokkos::Prod<int>(sign_det));

  Kokkos::parallel_reduce(view.extent(0), KOKKOS_LAMBDA (int ii, ValueType& v) {
      v += std::log(std::abs(ddk.psiM(i,i)));
    }, logdet);
  lah.getri(ddk.psiM);
  phase = (sign_det > 0) ? 0.0 : M_PI;
  //auto logValMirror = Kokkos::create_mirror_view(ddk.LogValue);
  //logValMirror(0) = locLogDet;
  //Kokkos::deep_copy(ddk.LogValue, logValMirror);
  return locLogDet;
}

template<class linAlgHelperType, typename ValueType>
void updateRow(DiracDeterminantKokkos& ddk, LinAlgHelperType& lah, int rowchanged, ValueType c_ratio_in) {
  constexpr ValueType cone(1.0);
  constexpr ValueType czero(0.0);
  ValueType c_ratio = cone / c_ratio_in;
  Kokkos::Profiling::PushRegion("updateRow::gemvTrans");
  lah.gemvTrans(ddk.psiMinv, ddk.psiV, ddk.tempRowVec, c_ratio, czero);
  Kokkos::Profiling::popRegion();

  // hard work to modify one element of temp on the device
  Kokkos::Profiling::pushRegion("updateRow::pokeSingleValue");
  auto devElem = subview(ddk.tempRowVec, rowchanged);
  auto devElem_mirror = Kokkos::create_mirror_view(devElem);
  devElem_mirror(0) = cone - c_ratio;
  Kokkos::deep_copy(devElem, devElem_mirror);
  Kokkos::Profiling::popRegion();

  Kokkos::Profiling::pushRegion("updateRow::populateRcopy");
  lah.copyChangedRow(rowchanged, ddk.psiMinv, ddk.rcopy);
  Kokkos::Profiling::popRegion();

  // now do ger
  Kokkos::Profiling::pushRegion("updateRow::ger");
  lah.ger(ddk.psiMinv, ddk.rcopy, ddk.tempRowVec, -cone);
  Kokkos::Profiling::popRegion();
}



// this design is problematic because I cannot call cublas from inside of a device function
// probably will have to abandon this and fold it back into the earlier version, but perhaps I
// can keep the scalar values on the device (in views).  Then I would keep the structure of the 
// code as it is and just add functionality to the linalgHelper to work in a particular stream and
// also to be able to set cublas to expect the scalar arguments to be in the device memory

// can actually still keep this.  The point is that I can have a method in DiracDeterminantKokkos
// that takes a view full of DiracDeterminantKokkos and then I can operate on them directly.

// also, I can use the embedded linalghelpers to do the low level work, but I will certainly need
// to template these functions over the memory space.
 

// need to reorganize in two ways
// 1.  push data out into plain class that can live on the device
//      also add needed functionality inline here for convenience
struct DiracDeterminantKokkos : public QMCTraits
{
  using MatType = Kokkos::View<ValueType**, Kokkos::LayoutRight>;
  using DoubleMatType = Kokkos::View<double**, Kokkos::LayoutRight>;

  Kokkos::View<ValueType[1]> LogValue;
  Kokkos::View<ValueType[1]> curRatio;
  Kokkos::View<int[1]> FirstIndex;

  // inverse matrix to be updated
  MatType psiMinv;
  // storage for the row update 
  Kokkos::View<ValueType*> psiV;
  // temporary storage for row update
  Kokkos::View<ValueType*> tempRowVec;
  Kokkos::View<ValueType*> rcopy;
  // internal storage to perform inversion correctly
  DoubleMatType psiM;
  // temporary workspace for inversion
  MatType psiMsave;

  KOKKOS_INLINE_FUNCTION
  DiracDeterminantKokkos() { ; }

  KOKKOS_INLINE_FUNCTION
  DiracDeterminantKokkos* operator=(const DiracDeterminantKokkos& rhs) {
    LogValue = rhs.LogValue;
    curRatio = rhs.curRatio;
    FirstIndex = rhs.FirstIndex;
    psiMinv = rhs.psiMinv;
    psiV = rhs.psiV;
    tempRowVec = rhs.tempRowVec;
    rcopy = rhs.rcopy;
    psiMsave = rhs.psiMsave;
    return this;
  }

  DiracDeterminantKokkos(const DiracDeterminantKokkos&) = default;
  // need to add in checkMatrix(), evaluateLog(psk*), evalGrad(psk*, iat),
  //                ratioGrad(psk*, iat, gradType), evaluateGL(psk*, G, L, fromscratch)
  //                recompute(), ratio(psk*, iel), acceptMove(psk*, iel)
};
      

struct DiracDeterminant : public WaveFunctionComponent
{
  DiracDeterminant(int nels, const RandomGenerator<RealType>& RNG, int First = 0) 
    : FirstIndex(First), myRandom(RNG)
  {
    ddk.LogValue   = Kokkos::View<ValueType[1]>("LogValue");
    ddk.curRatio   = Kokkos::View<ValueType[1]>("curRatio");
    ddk.FirstIndex = Kokkos::View<int[1]>("FirstIndex");
    ddk.psiMinv    = DiracDeterminantKokkos::MatType("psiMinv",nels,nels);
    ddk.psiM       = DiracDeterminantKokkos::DoubleMatType("psiM",nels,nels);
    ddk.psiMsave   = DiracDeterminantKokkos::MatType("psiMsave",nels,nels);
    ddk.psiV       = Kokkos::View<ValueType*>("psiV",nels);
    ddk.tempRowVec = Kokkos::View<ValueType*>("tempRowVec", nels);
    ddk.rcopy      = Kokkos::View<ValueType*>("rcopy", nels);

    FirstIndexMirror = Kokkos::create_mirror_view(ddk.FirstIndex);
    FirstIndexMirror(0) = FirstIndex;
    Kokkos::deep_copy(ddk.FirstIndex, FirstIndexMirror);
    
    // basically we are generating uniform random number for
    // each entry of psiMsave in the interval [-0.5, 0.5]
    constexpr double shift(0.5);

    // change this to match data ordering of DeterminantRef
    // recall that psiMsave has as its fast index the leftmost index
    // however int the c style matrix in DeterminantRef 
    auto psiMsaveMirror = Kokkos::create_mirror_view(psiMsave);
    auto psiMMirror = Kokkos::create_mirror_view(psiM);
    for (int i = 0; i < nels; i++) {
      for (int j = 0; j < nels; j++) {
	psiMsaveMirror(i,j) = myRandom.rand()-shift;
	psiMMirror(j,i) = psiMsaveMirror(i,j);
      }
    }
    Kokkos::deep_copy(psiMsave, psiMsaveMirror);
    Kokkos::deep_copy(psiM, psiMMirror);

    RealType phase;
    LogValue = InvertWithLog(ddk, lah, phase);
    elementWiseCopy(psiMinv, psiM);
  }

  void checkMatrix()
  {
    DiradDeterminantBase::MatType psiMRealType("psiM_RealType", ddk.psiM.extent(0), ddk.psiM.extent(0));
    elementWiseCopy(psiMRealType, ddk.psiM);
    checkIdentity(ddk.psiMsave, psiMRealType, "Psi_0 * psiM(T)", lah);
    checkIdentity(ddk.psiMsave, ddk.psiMinv, "Psi_0 * psiMinv(T)", lah);
    checkDiff(psiMRealType, ddk.psiMinv, "psiM - psiMinv(T)");
  }

  RealType evaluateLog(ParticleSet& P,
		       ParticleSet::ParticleGradient_t& G,
		       ParticleSet::ParticleLaplacian_t& L)
  {
    recompute();
    return 0.0;
  }

  GradType evalGrad(ParticleSet& P, int iat) { return GradType(); }
  ValueType ratioGrad(ParticleSet& P, int iat, GradType& grad) { return ratio(P, iat); }
  void evaluateGL(ParticleSet& P,
                  ParticleSet::ParticleGradient_t& G,
                  ParticleSet::ParticleLaplacian_t& L,
                  bool fromscratch = false) {}

  inline void recompute()
  {
    //elementWiseCopy(psiM, psiMsave); // needs to be transposed!
    elementWiseCopyTrans(ddk.psiM, ddk.psiMsave); // needs to be transposed!
    lah.invertMatrix(ddk.psiM);
    elementWiseCopy(ddk.psiMinv, ddk.psiM);
  }

  // in real application, inside here it would actually evaluate spos at the new
  // position and stick them in psiV
  inline ValueType ratio(ParticleSet& P, int iel)
  {
    const int nels = ddk.psiV.extent(0);
    constexpr double shift(0.5);
    //constexpr double czero(0);

    auto psiVMirror = Kokkos::create_mirror_view(psiV);
    for (int j = 0; j < nels; ++j) {
      psiVMirror(j) = myRandom() - shift;
    }
    Kokkos::deep_copy(ddk.psiV, psiVMirror);
    // in main line previous version this looked like:
    // curRatio = inner_product_n(psiV.data(), psiMinv[iel - FirstIndex], nels, czero);
    // same issues with indexing
    curRatio = lah.updateRatio(ddk.psiV, ddk.psiMinv, iel-FirstIndex);
    return curRatio;
  }
  inline void acceptMove(ParticleSet& P, int iel) {
    Kokkos::Profiling::pushRegion("Determinant::acceptMove");
    const int nels = ddk.psiV.extent(0);
    
    Kokkos::Profiling::pushRegion("Determinant::acceptMove::updateRow");
    updateRow(ddk.psiMinv, ddk.psiV, iel-FirstIndex, curRatio, lah);
    Kokkos::Profiling::popRegion();
    // in main line previous version this looked like:
    //std::copy_n(psiV.data(), nels, psiMsave[iel - FirstIndex]);
    // note 1: copy_n copies data from psiV to psiMsave
    //
    // note 2: the single argument call to psiMsave[] returned a pointer to
    // the iel-FirstIndex ROW of the underlying data structure, so
    // the operation was like (psiMsave.data() + (iel-FirstIndex)*D2)
    // note that then to iterate through the data it was going sequentially
    Kokkos::Profiling::pushRegion("Determinant::acceptMove::copyBack");
    lah.copyBack(ddk.psiMsave, ddk.psiV, iel-FirstIndex);

    /*
    const int FirstIndex_ = FirstIndex;
    Kokkos::View<RealType*> psiV_=psiV;
    const int iel_=iel;
    DoubleMatType psiMsave_=psiMsave; 



    Kokkos::parallel_for( nels, KOKKOS_LAMBDA (int i) {
    	psiMsave_(iel_-FirstIndex_, i) = psiV_(i);
      });
    Kokkos::fence();
    */
    Kokkos::Profiling::popRegion();
    Kokkos::Profiling::popRegion();
  }

  // accessor functions for checking
  inline double operator()(int i) const {
    // not sure what this was for, seems odd to 
    //Kokkos::deep_copy(psiMinv, psiMinv_host);
    int x = i / ddk.psiMinv_host.extent(0);
    int y = i % ddki.psiMinv_host.extent(0);
    auto dev_subview = subview(ddk.psiMinv, x, y);
    auto dev_subview_host = Kokkos::create_mirror_view(dev_subview);
    Kokkos::deep_copy(dev_subview_host, dev_subview);
    return dev_subview_host(0,0);
  }
  inline int size() const { return psiMinv.extent(0)*psiMinv.extent(1); }

  DiracDeterminantKokkos ddk;
private:
  /// log|det|
  double LogValue;
  /// current ratio
  double curRatio;
  /// initial particle index
  const int FirstIndex;
  /// random number generator for testing
  RandomGenerator<RealType> myRandom;
  /// Helper class to handle linear algebra
  /// Holds for instance space for pivots and workspace
  linalgHelper<ValueType, DiracDeterminantKokkos::array_layout, DiracDeterminantKokkos::memory_space> lah;
};
} // namespace qmcplusplus







#endif
