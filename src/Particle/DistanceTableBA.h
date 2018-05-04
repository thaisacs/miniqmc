////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source
// License.  See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by:
// Jeongnim Kim, jeongnim.kim@intel.com,
//    Intel Corp.
// Amrita Mathuriya, amrita.mathuriya@intel.com,
//    Intel Corp.
//
// File created by:
// Jeongnim Kim, jeongnim.kim@intel.com,
//    Intel Corp.
////////////////////////////////////////////////////////////////////////////////
// -*- C++ -*-
#ifndef QMCPLUSPLUS_DTDIMPL_BA_H
#define QMCPLUSPLUS_DTDIMPL_BA_H

namespace qmcplusplus
{
/**@ingroup nnlist
 * @brief A derived classe from DistacneTableData, specialized for AB using a
 * transposed form
 */
template <typename T, unsigned D, int SC>
struct DistanceTableBA : public DTD_BConds<T, D, SC>, public DistanceTableData
{
  int Nsources;
  int Ntargets;
  int BlockSize;

  DistanceTableBA(const ParticleSet &source, ParticleSet &target)
      : DTD_BConds<T, D, SC>(source.Lattice), DistanceTableData(source, target)
  {
    resize(source.getTotalNum(), target.getTotalNum());
  }

  void resize(int ns, int nt)
  {
    N[SourceIndex] = Nsources = ns;
    N[VisitorIndex] = Ntargets = nt;
    if (Nsources * Ntargets == 0) return;

    int Ntargets_padded = getAlignedSize<T>(Ntargets);
    int Nsources_padded = getAlignedSize<T>(Nsources);

    Distances.resize(Ntargets, Nsources_padded);

    BlockSize = Nsources_padded * D;
    memoryPool.resize(Ntargets * BlockSize);
    Displacements.resize(Ntargets);
    for (int i = 0; i < Ntargets; ++i)
      Displacements[i].attachReference(Nsources, Nsources_padded,
                                       memoryPool.data() + i * BlockSize);

    Temp_r.resize(Nsources);
    Temp_dr.resize(Nsources);

    // this is used to build a compact list
    M.resize(Nsources);
    r_m2.resize(Nsources, Ntargets_padded);
    dr_m2.resize(Nsources, Ntargets_padded);
    J2.resize(Nsources, Ntargets_padded);
  }

  DistanceTableBA()                        = delete;
  DistanceTableBA(const DistanceTableBA &) = delete;
  ~DistanceTableBA() {}

  /** evaluate the full table */
  inline void evaluate(ParticleSet &P)
  {
    // be aware of the sign of Displacement
    for (int iat = 0; iat < Ntargets; ++iat)
      DTD_BConds<T, D, SC>::computeDistances(P.R[iat], Origin->RSoA,
                                             Distances[iat], Displacements[iat],
                                             0, Nsources);
  }

  /** evaluate the iat-row with the current position
   *
   * Fill Temp_r and Temp_dr and copy them Distances & Displacements
   */
  inline void evaluate(ParticleSet &P, IndexType iat)
  {
    DTD_BConds<T, D, SC>::computeDistances(P.R[iat], Origin->RSoA,
                                           Distances[iat], Displacements[iat],
                                           0, Nsources);
  }

  inline void moveOnSphere(const ParticleSet &P, const PosType &rnew)
  {
    DTD_BConds<T, D, SC>::computeDistances(rnew, Origin->RSoA, Temp_r.data(),
                                           Temp_dr, 0, Nsources);
  }

  /// evaluate the temporary pair relations
  inline void move(const ParticleSet &P, const PosType &rnew)
  {
    DTD_BConds<T, D, SC>::computeDistances(rnew, Origin->RSoA, Temp_r.data(),
                                           Temp_dr, 0, Nsources);
  }

  /// update the stripe for jat-th particle
  inline void update(IndexType iat)
  {
    simd::copy_n(Temp_r.data(), Nsources, Distances[iat]);
    for (int idim = 0; idim < D; ++idim)
      simd::copy_n(Temp_dr.data(idim), Nsources, Displacements[iat].data(idim));
  }

  inline void donePbyP()
  {
    // Rmax is zero: no need to transpose the table.
    if (Rmax < std::numeric_limits<T>::epsilon()) return;

    CONSTEXPR T cminus(-1);
    for (int iat = 0; iat < Nsources; ++iat)
    {
      int nn                  = 0;
      int *restrict jptr      = J2[iat];
      RealType *restrict rptr = r_m2[iat];
      PosType *restrict dptr  = dr_m2[iat];
      for (int jat = 0; jat < Ntargets; ++jat)
      {
        const RealType rij = Distances[jat][iat];
        if (rij < Rmax)
        { // make the compact list
          rptr[nn] = rij;
          dptr[nn] = cminus * Displacements[jat][iat];
          jptr[nn] = jat;
          nn++;
        }
      }
      M[iat] = nn;
    }
  }
};
} // namespace qmcplusplus
#endif
