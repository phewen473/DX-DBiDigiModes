! DX-DBi v2R0: delta encoding + incremental bucket reset (see mrbdeltawspr, boxit)
subroutine osdwspr(ss,apmask,ndeep,cw,nhardmin,dmin)
! 
use iso_c_binding
parameter (N=162, K=50, L=32)

!integer*1 p1(L),p2(L),p3(L),p4(L)
integer*1 gg(64)

real ss(N)
integer*1 apmask(N),apmaskr(N)
integer*1 gen(K,N)
integer*1 genmrb(K,N),g2(N,K)
integer*1 temp(K),m0(K),me(K),mi(K),misub(K),e2sub(N-K),e2(N-K),ui(N-K)
integer*1 r2pat(N-K)
integer indices(N),nxor(N)
integer*1 cw(N),ce(N),c0(N),hdec(N)
integer indx(N),ndeep,nhardmin
real rx(N),absrx(N),dmin
logical first,reset
logical apsub
integer*8 gb1(K),gb2(K),eb1,eb2
integer gk(K),ek,ipat2
data first/.true./
data gg/1,1,0,1,0,1,0,0,1,0,0,0,1,1,0,0,1,0,1,0,0,1,0,1,1,1,0,1,1,0,0,0, &
        0,1,0,0,0,0,0,0,1,0,0,1,1,1,1,0,0,0,1,0,0,1,0,0,1,0,1,1,1,1,1,1/

save first,gen

if( first ) then ! fill the generator matrix
  gen=0
  gen(1,1:2*L)=gg(1:2*L)
  do i=2,K
    gen(i,:)=cshift(gen(i-1,:),-2)
  enddo
  first=.false.
endif

rx=ss/127.0
apmaskr=apmask

! Hard decisions on the received word.
hdec=0            
where(rx .ge. 0) hdec=1

! Use magnitude of received symbols as a measure of reliability.
absrx=abs(rx) 
call indexx(absrx,N,indx)  

! Re-order the columns of the generator matrix in order of decreasing reliability.
do i=1,N
  genmrb(1:K,i)=gen(1:K,indx(N+1-i))
  indices(i)=indx(N+1-i)
enddo

! Do gaussian elimination to create a generator matrix with the most reliable
! received bits in positions 1:K in order of decreasing reliability (more or less). 
do id=1,K ! diagonal element indices 
  do icol=id,K+20  ! The 20 is ad hoc - beware
    iflag=0
    if( genmrb(id,icol) .eq. 1 ) then
      iflag=1
      if( icol .ne. id ) then ! reorder column
        temp(1:K)=genmrb(1:K,id)
        genmrb(1:K,id)=genmrb(1:K,icol)
        genmrb(1:K,icol)=temp(1:K) 
        itmp=indices(id)
        indices(id)=indices(icol)
        indices(icol)=itmp
      endif
      do ii=1,K
        if( ii .ne. id .and. genmrb(ii,id) .eq. 1 ) then
          genmrb(ii,1:N)=ieor(genmrb(ii,1:N),genmrb(id,1:N))
        endif
      enddo
      exit
    endif
  enddo
enddo

g2=transpose(genmrb)

! The hard decisions for the K MRB bits define the order 0 message, m0. 
! Encode m0 using the modified generator matrix to find the "order 0" codeword. 
! Flip various combinations of bits in m0 and re-encode to generate a list of
! codewords. Return the member of the list that has the smallest Euclidean
! distance to the received word. 

hdec=hdec(indices)   ! hard decisions from received symbols
m0=hdec(1:K)         ! zero'th order message
absrx=absrx(indices) 
rx=rx(indices)       
apmaskr=apmaskr(indices)

call mrbencode(m0,c0,g2,N,K)

nxor=ieor(c0,hdec)
nhardmin=sum(nxor)
dmin=sum(nxor*absrx)
cw=c0
ntotal=0
nrejected=0

if(ndeep.le.0) goto 998  ! norder=0
if(ndeep.gt.5) ndeep=5
if( ndeep.eq. 1) then
   nord=1
   npre1=0
   npre2=0
   nt=66
   ntheta=16
elseif(ndeep.eq.2) then
   nord=1
   npre1=1
   npre2=0
   nt=66
   ntheta=22
   ntau = 16
elseif(ndeep.eq.3) then
   nord=1
   npre1=1
   npre2=1
   nt=66
   ntheta=22
   ntau=16
elseif(ndeep.eq.4) then
   nord=2
   npre1=1
   npre2=1
   nt=66
   ntheta=22
   ntau=16
elseif(ndeep.eq.5) then
   nord=3
   npre1=1
   npre2=1
   nt=66
   ntheta=22
   ntau=16
endif

! DX-DBi v2R0: the first nt parity bits of each column of g2 packed into
! two 64-bit words, so the test "sum(e2(1:nt))" for a candidate pattern is
! two XORs and two popcounts instead of ~400 byte operations. The count is
! exactly the same, so the same patterns are accepted and rejected.
nt1=min(nt,64)
nt2=min(nt-nt1,64)
do i=1,K
   gb1(i)=0_8
   gb2(i)=0_8
   do j=1,nt1
      if(g2(K+j,i).eq.1) gb1(i)=ibset(gb1(i),j-1)
   enddo
   do j=1,nt2
      if(g2(K+64+j,i).eq.1) gb2(i)=ibset(gb2(i),j-1)
   enddo
enddo
eb1=0_8
eb2=0_8

do iorder=1,nord
   misub(1:K-iorder)=0
   misub(K-iorder+1:K)=1
   iflag=K-iorder+1
   do while(iflag .ge.0)
      if(iorder.eq.nord .and. npre1.eq.0) then
         iend=iflag
      else
         iend=1
      endif
      d1=0.
      apsub=any(iand(apmaskr(1:K),misub).eq.1)   ! same as the per-pattern test below
      do n1=iflag,iend,-1
         if(apsub .or. apmaskr(n1).eq.1) cycle     ! any(iand(apmaskr,mi).eq.1)
         ntotal=ntotal+1
         if(n1.eq.iflag) then
            mi=misub
            me=ieor(m0,mi)
            call mrbdeltawspr(c0,mi,ce,g2,N,K)
            e2sub=ieor(ce(K+1:N),hdec(K+1:N))
            e2=e2sub
            nd1Kpt=sum(e2sub(1:nt))+1
            d1=sum(ieor(me(1:K),hdec(1:K))*absrx(1:K))
            eb1=0_8
            eb2=0_8
            do j=1,nt1
               if(e2sub(j).eq.1) eb1=ibset(eb1,j-1)
            enddo
            do j=1,nt2
               if(e2sub(64+j).eq.1) eb2=ibset(eb2,j-1)
            enddo
         else
            nd1Kpt=popcnt(ieor(eb1,gb1(n1)))+popcnt(ieor(eb2,gb2(n1)))+2
         endif
         if(nd1Kpt .le. ntheta) then
            if(n1.ne.iflag) then
               mi=misub
               mi(n1)=1
               e2=ieor(e2sub,g2(K+1:N,n1))
               call mrbdeltawspr(c0,mi,ce,g2,N,K)
            endif
            nxor=ieor(ce,hdec)
            if(n1.eq.iflag) then
               dd=d1+sum(e2sub*absrx(K+1:N))
            else
               dd=d1+ieor(ce(n1),hdec(n1))*absrx(n1)+sum(e2*absrx(K+1:N))
            endif
            if( dd .lt. dmin ) then
               dmin=dd
               cw=ce
               nhardmin=sum(nxor)
               nd1Kptbest=nd1Kpt
            endif
         else 
            nrejected=nrejected+1
         endif
      enddo
! Get the next test error pattern, iflag will go negative
! when the last pattern with weight iorder has been generated.
      call nextpat(misub,k,iorder,iflag)
   enddo
enddo

if(npre2.eq.1) then
! DX-DBi v2R0: the ntau-bit bucket key of a bit pattern p(1:ntau) is
! sum p(j)*2**(ntau-j). Keys are built once per column / per misub and the
! single-bit change ui(i2) is a single XOR, instead of rebuilding two
! 112-byte arrays and the key bit by bit for each of the ~20,000 lookups.
! Same keys, same lookups, same order => same result.
   do i=1,K
      gk(i)=0
      do j=1,ntau
         if(g2(K+j,i).eq.1) gk(i)=ibset(gk(i),ntau-j)
      enddo
   enddo
   reset=.true.
   ntotal=0
   do i1=K,1,-1
      do i2=i1-1,1,-1
         ntotal=ntotal+1
         call boxit(reset,ieor(gk(i1),gk(i2)),ntotal,i1,i2)
      enddo
   enddo

   ncount2=0
   ntotal2=0
   reset=.true.
! Now run through again and do the second pre-processing rule
   misub(1:K-nord)=0
   misub(K-nord+1:K)=1
   iflag=K-nord+1
   do while(iflag .ge.0)
      me=ieor(m0,misub)
      call mrbdeltawspr(c0,misub,ce,g2,N,K)
      e2sub=ieor(ce(K+1:N),hdec(K+1:N))
      ek=0
      do j=1,ntau
         if(e2sub(j).eq.1) ek=ibset(ek,ntau-j)
      enddo
      do i2=0,ntau
         ntotal2=ntotal2+1
         ipat2=ek                                  ! key of r2pat=ieor(e2sub,ui)
         if(i2.gt.0) ipat2=ieor(ek,ishft(1,ntau-i2))
778      continue 
            call fetchit(reset,ipat2,in1,in2)
            if(in1.gt.0.and.in2.gt.0) then
               ncount2=ncount2+1
               mi=misub               
               mi(in1)=1
               mi(in2)=1
               if(sum(mi).lt.nord+npre1+npre2.or.any(iand(apmaskr(1:K),mi).eq.1)) cycle
               me=ieor(m0,mi)
               call mrbdeltawspr(c0,mi,ce,g2,N,K)
               nxor=ieor(ce,hdec)
               dd=sum(nxor*absrx)
               if( dd .lt. dmin ) then
                  dmin=dd
                  cw=ce
                  nhardmin=sum(nxor)
               endif
               goto 778
             endif
      enddo
      call nextpat(misub,K,nord,iflag)
   enddo
endif

998 continue
! Re-order the codeword to as-received order.
cw(indices)=cw
hdec(indices)=hdec
return
end subroutine osdwspr

subroutine mrbencode(me,codeword,g2,N,K)
integer*1 me(K),codeword(N),g2(N,K)
! fast encoding for low-weight test patterns
  codeword=0
  do i=1,K
    if( me(i) .eq. 1 ) then
      codeword=ieor(codeword,g2(1:N,i))
    endif
  enddo
return
end subroutine mrbencode

subroutine nextpat(mi,k,iorder,iflag)
  integer*1 mi(k),ms(k)
! generate the next test error pattern
  ind=-1
  do i=1,k-1
     if( mi(i).eq.0 .and. mi(i+1).eq.1) ind=i 
  enddo
  if( ind .lt. 0 ) then ! no more patterns of this order
    iflag=ind
    return
  endif
  ms=0
  ms(1:ind-1)=mi(1:ind-1)
  ms(ind)=1
  ms(ind+1)=0
  if( ind+1 .lt. k ) then
     nz=iorder-sum(ms)
     ms(k-nz+1:k)=1
  endif
  mi=ms
  do i=1,k  ! iflag will point to the lowest-index 1 in mi
    if(mi(i).eq.1) then
      iflag=i 
      exit
    endif
  enddo
  return
end subroutine nextpat

subroutine boxit(reset,ipat,npindex,i1,i2)
! DX-DBi v2R0: takes the bucket key ipat (was built here from e2(1:ntau))
   logical boxinit
   data boxinit/.false./
   save boxinit
  integer   indexes(4000,2),fp(0:525000),np(4000),nused,iused(4000)
  logical reset
  common/boxeswspr/indexes,fp,np,nused,iused
  ! DX-DBi v2R0: clear the whole bucket table once, then only the
  ! buckets filled since the last reset (was 2.1 MB per call).
  if(.not.boxinit) then
    fp=-1
    nused=0
    boxinit=.true.
  endif
  if(reset) then
    do i=1,nused
      fp(iused(i))=-1
    enddo
    nused=0
    np=-1
    indexes=-1
    reset=.false.
  endif
 
  indexes(npindex,1)=i1
  indexes(npindex,2)=i2

  ip=fp(ipat)   ! see what's currently stored in fp(ipat)
  if(ip.eq.-1) then
    fp(ipat)=npindex
    nused=nused+1
    iused(nused)=ipat
  else
     do while (np(ip).ne.-1)
      ip=np(ip) 
     enddo  
     np(ip)=npindex
  endif
  return
end subroutine boxit

subroutine fetchit(reset,ipat,i1,i2)
! DX-DBi v2R0: takes the bucket key ipat (was built here from e2(1:ntau))
  integer   indexes(4000,2),fp(0:525000),np(4000),nused,iused(4000)
  integer   lastpat
  logical reset
  common/boxeswspr/indexes,fp,np,nused,iused
  save lastpat,inext

  if(reset) then
    lastpat=-1
    reset=.false.
  endif

  index=fp(ipat)

  if(lastpat.ne.ipat .and. index.gt.0) then ! return first set of indices
     i1=indexes(index,1)
     i2=indexes(index,2)
     inext=np(index)
  elseif(lastpat.eq.ipat .and. inext.gt.0) then
     i1=indexes(inext,1)
     i2=indexes(inext,2)
     inext=np(inext)
  else
     i1=-1
     i2=-1
     inext=-1
  endif
  lastpat=ipat
  return
end subroutine fetchit
 

subroutine mrbdeltawspr(c0,mi,codeword,g2,N,K)
! DX-DBi v2R0: codeword of (m0 xor mi) = c0 xor columns of g2 where mi=1.
! Same bits as mrbencode(ieor(m0,mi)) but only touches the 1-4 set columns.
  integer*1 c0(N),mi(K),codeword(N),g2(N,K)
  codeword=c0
  do i=1,K
    if( mi(i) .eq. 1 ) then
      codeword=ieor(codeword,g2(1:N,i))
    endif
  enddo
  return
end subroutine mrbdeltawspr
