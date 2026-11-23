subroutine four2a(a,nfft,ndim,isign,iform)

! IFORM = 1, 0 or -1, as data is
! complex, real, or the first half of a complex array.  Transform
! values are returned in array DATA.  They are complex, real, or
! the first half of a complex array, as IFORM = 1, -1 or 0.

! The transform of a real array (IFORM = 0) dimensioned N(1) by N(2)
! by ... will be returned in the same array, now considered to
! be complex of dimensions N(1)/2+1 by N(2) by ....  Note that if
! IFORM = 0 or -1, N(1) must be even, and enough room must be
! reserved.  The missing values may be obtained by complex conjugation.  

! The reverse transformation of a half complex array dimensioned
! N(1)/2+1 by N(2) by ..., is accomplished by setting IFORM
! to -1.  In the N array, N(1) must be the true N(1), not N(1)/2+1.
! The transform will be real and returned to the input array.

! This version of four2a makes calls to the FFTW library to do the 
! actual computations.

  use fftw3
  parameter (NPMAX=2100)                 !Max number of stored plans
  parameter (NSMALL=16385)               !Max half complex size of "small" FFTs
  complex a(nfft)                        !Array to be transformed
  complex aa(NSMALL)                     !Local copy of "small" a()
  complex, allocatable :: abig(:)        !Local copy of "big" a() while planning
  integer nn(NPMAX),ns(NPMAX),nf(NPMAX)  !Params of stored plans
  integer*8 nl(NPMAX),nalign             !Alignment of the planned arrays
  integer*8 plan(NPMAX)                  !Pointers to stored plans
  integer*8 iplan
  logical found_plan
  data nplan/0/                          !Number of stored plans
  common/patience/npatience,nthreads     !Patience and threads for FFTW plans
  save plan,nplan,nn,ns,nf,nl

! DX-DBi: plans are keyed on (size, direction, type, alignment) rather than
! on the address of the array, and executed with FFTW's thread-safe
! "new-array" calls.  One plan now serves every array with the same layout
! (and every thread), lookup and creation happen under one lock (the old
! four2avar searched without it), and arrays larger than NSMALL are saved
! and restored around planning so FFTW_MEASURE etc. cannot destroy the data.

  if(nfft.lt.0) go to 999

  nalign=mod(loc(a),64_8)               !FFTW requires identical alignment
  found_plan = .false.
  !$omp critical(four2a_setup)
  do i=1,nplan
     if(nfft.eq.nn(i) .and. isign.eq.ns(i) .and.                     &
          iform.eq.nf(i) .and. nalign.eq.nl(i)) then
        found_plan = .true.
        exit
     end if
  enddo

  if (.not. found_plan) then
     if(nplan.ge.NPMAX) stop 'Too many FFTW plans requested.'
     nplan=nplan+1
     i=nplan

     nn(i)=nfft
     ns(i)=isign
     nf(i)=iform
     nl(i)=nalign

! Planning: FFTW_ESTIMATE, FFTW_ESTIMATE_PATIENT, FFTW_MEASURE,
!            FFTW_PATIENT,  FFTW_EXHAUSTIVE
     nflags=FFTW_ESTIMATE
     if(npatience.eq.1) nflags=FFTW_ESTIMATE_PATIENT
     if(npatience.eq.2) nflags=FFTW_MEASURE
     if(npatience.eq.3) nflags=FFTW_PATIENT
     if(npatience.eq.4) nflags=FFTW_EXHAUSTIVE

     jz=nfft
     if(iform.le.0) jz=nfft/2+1
     if(jz.le.NSMALL) then
        aa(1:jz)=a(1:jz)
     else
        allocate(abig(jz))
        abig(1:jz)=a(1:jz)
     endif

     !$omp critical(fftw) ! serialize non thread-safe FFTW3 calls
     if(isign.eq.-1 .and. iform.eq.1) then
        call sfftw_plan_dft_1d(plan(i),nfft,a,a,FFTW_FORWARD,nflags)
     else if(isign.eq.1 .and. iform.eq.1) then
        call sfftw_plan_dft_1d(plan(i),nfft,a,a,FFTW_BACKWARD,nflags)
     else if(isign.eq.-1 .and. iform.eq.0) then
        call sfftw_plan_dft_r2c_1d(plan(i),nfft,a,a,nflags)
     else if(isign.eq.1 .and. iform.eq.-1) then
        call sfftw_plan_dft_c2r_1d(plan(i),nfft,a,a,nflags)
     else
        stop 'Unsupported request in four2a'
     endif
     !$omp end critical(fftw)

     if(jz.le.NSMALL) then
        a(1:jz)=aa(1:jz)
     else
        a(1:jz)=abig(1:jz)
        deallocate(abig)
     endif
  end if
  iplan=plan(i)
  !$omp end critical(four2a_setup)

  if(iform.eq.1) then
     call sfftw_execute_dft(iplan,a,a)
  else if(iform.eq.0) then
     call sfftw_execute_dft_r2c(iplan,a,a)
  else
     call sfftw_execute_dft_c2r(iplan,a,a)
  endif
  return

999 continue

  !$omp critical(four2a_setup)
  do i=1,nplan
! The test is only to silence a compiler warning:
     if(ndim.ne.-999) then
        !$omp critical(fftw) ! serialize non thread-safe FFTW3 calls
        call sfftw_destroy_plan(plan(i))
        !$omp end critical(fftw)
     end if
  enddo
  nplan=0
  !$omp end critical(four2a_setup)

  return
end subroutine four2a
