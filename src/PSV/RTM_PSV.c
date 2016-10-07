/*
 * Elastic Reverse Time Migration RTM (2D PSV problem)  
 *
 * Daniel Koehn
 * Kiel, 06/06/2016
 */

#include "fd.h"

void RTM_PSV(){

/* global variables */
/* ---------------- */

/* forward modelling */
extern int MYID, FDORDER, NX, NY, NT, L, READMOD, QUELLART, RUN_MULTIPLE_SHOTS, TIME_FILT, READREC;
extern int LOG, SEISMO, N_STREAMER, FW, NXG, NYG, IENDX, IENDY, NTDTINV, IDXI, IDYI, NXNYI, INV_STF, DTINV;
extern float FC_SPIKE_1, FC_SPIKE_2, FC, FC_START, TIME, DT;
extern char LOG_FILE[STRING_SIZE], MFILE[STRING_SIZE];
extern FILE *FP;

/* gravity modelling/inversion */
extern int GRAVITY, NZGRAV, NGRAVB, GRAV_TYPE, BACK_DENSITY;
extern char GRAV_DATA_OUT[STRING_SIZE], GRAV_DATA_IN[STRING_SIZE], GRAV_STAT_POS[STRING_SIZE], DFILE[STRING_SIZE];
extern float LAM_GRAV, GAMMA_GRAV, LAM_GRAV_GRAD, L2_GRAV_IT1;

/* full waveform inversion */
extern int GRAD_METHOD, NLBFGS, ITERMAX, IDX, IDY, INVMAT1, EPRECOND;
extern int GRAD_FORM, POS[3], QUELLTYPB, MIN_ITER, MODEL_FILTER;
extern float FC_END, PRO, C_vp, C_vs, C_rho;
extern char MISFIT_LOG_FILE[STRING_SIZE], JACOBIAN[STRING_SIZE];
extern char *FILEINP1;

/* local variables */
int ns, nseismograms=0, nt, nd, fdo3, j, i, iter, h, hin, iter_true, SHOTINC, s=0;
int buffsize, ntr=0, ntr_loc=0, ntr_glob=0, nsrc=0, nsrc_loc=0, nsrc_glob=0, ishot, nshots=0, itestshot;

float sum, eps_scale, opteps_vp, opteps_vs, opteps_rho, Vp_avg, Vs_avg, rho_avg, Vs_sum, Vp_sum, rho_sum;
char *buff_addr, ext[10], *fileinp, jac[225], source_signal_file[STRING_SIZE];

double time1, time2, time7, time8, time_av_v_update=0.0, time_av_s_update=0.0, time_av_v_exchange=0.0, time_av_s_exchange=0.0, time_av_timestep=0.0;
	
float L2sum, *L2t;
	
float ** taper_coeff, * epst1, *hc=NULL;
int * DTINV_help;

MPI_Request *req_send, *req_rec;
MPI_Status  *send_statuses, *rec_statuses;

/* Variables for step length calculation */
int step1, step3=0;
float eps_true, tmp;

/* Variables for the L-BFGS method */
float * rho_LBFGS, * alpha_LBFGS, * beta_LBFGS; 
float * y_LBFGS, * s_LBFGS, * q_LBFGS, * r_LBFGS;
int NLBFGS_class, LBFGS_pointer, NLBFGS_vec;

/* Variables for energy weighted gradient */
float ** Ws, **Wr, **We;

/* parameters for FWI-workflow */
int stagemax=0, nstage;

/* help variable for MIN_ITER */
int min_iter_help=0;

/* parameters for gravity inversion */
char jac_grav[STRING_SIZE];

FILE *FP_stage, *FP_GRAV;

if (MYID == 0){
   time1=MPI_Wtime(); 
   clock();
}

/* open log-file (each PE is using different file) */
/*	fp=stdout; */
sprintf(ext,".%i",MYID);  
strcat(LOG_FILE,ext);

if ((MYID==0) && (LOG==1)) FP=stdout;
else FP=fopen(LOG_FILE,"w");
fprintf(FP," This is the log-file generated by PE %d \n\n",MYID);

/* ----------------------- */
/* define FD grid geometry */
/* ----------------------- */

/* domain decomposition */
initproc();

NT=iround(TIME/DT); /* number of timesteps */

/* output of parameters to log-file or stdout */
if (MYID==0) write_par(FP);

/* NXG, NYG denote size of the entire (global) grid */
NXG=NX;
NYG=NY;

/* In the following, NX and NY denote size of the local grid ! */
NX = IENDX;
NY = IENDY;

NTDTINV=ceil((float)NT/(float)DTINV);		/* round towards next higher integer value */

/* save every IDXI and IDYI spatial point during the forward modelling */
IDXI=1;
IDYI=1;

NXNYI=(NX/IDXI)*(NY/IDYI);
SHOTINC=1;

/* use only every DTINV time sample for the inversion */
DTINV_help=ivector(1,NT);

/* Check if RTM workflow-file is defined (stdin) */
FP_stage=fopen(FILEINP1,"r");
if(FP_stage==NULL) {
	if (MYID == 0){
		printf("\n==================================================================\n");
		printf(" Cannot open Denise workflow input file %s \n",FILEINP1);
		printf("\n==================================================================\n\n");
		err(" --- ");
	}
}

fclose(FP_stage);

/* define data structures for PSV problem */
struct wavePSV;
struct wavePSV_PML;
struct matPSV;
struct fwiPSV;
struct mpiPSV;
struct seisPSV;
struct seisPSVfwi;
struct acq;

nd = FDORDER/2 + 1;
fdo3 = 2*nd;
buffsize=2.0*2.0*fdo3*(NX +NY)*sizeof(MPI_FLOAT);

/* allocate buffer for buffering messages */
buff_addr=malloc(buffsize);
if (!buff_addr) err("allocation failure for buffer for MPI_Bsend !");
MPI_Buffer_attach(buff_addr,buffsize);

/* allocation for request and status arrays */
req_send=(MPI_Request *)malloc(REQUEST_COUNT*sizeof(MPI_Request));
req_rec=(MPI_Request *)malloc(REQUEST_COUNT*sizeof(MPI_Request));
send_statuses=(MPI_Status *)malloc(REQUEST_COUNT*sizeof(MPI_Status));
rec_statuses=(MPI_Status *)malloc(REQUEST_COUNT*sizeof(MPI_Status));

/* --------- add different modules here ------------------------ */
ns=NT;	/* in a FWI one has to keep all samples of the forward modeled data
	at the receiver positions to calculate the adjoint sources and to do 
	the backpropagation; look at function saveseis_glob.c to see that every
	NDT sample for the forward modeled wavefield is written to su files*/

if (SEISMO&&(READREC!=2)){

   acq.recpos=receiver(FP, &ntr, ishot);
   acq.recswitch = ivector(1,ntr);
   acq.recpos_loc = splitrec(acq.recpos,&ntr_loc, ntr, acq.recswitch);
   ntr_glob=ntr;
   ntr=ntr_loc;
   
   if(N_STREAMER>0){
     free_imatrix(acq.recpos,1,3,1,ntr_glob);
     if(ntr>0) free_imatrix(acq.recpos_loc,1,3,1,ntr);
     free_ivector(acq.recswitch,1,ntr_glob);
   }
   
}

if((N_STREAMER==0)&&(READREC!=2)){

   /* Memory for seismic data */
   alloc_seisPSV(ntr,ns,&seisPSV);

   /* Memory for FWI seismic data */ 
   alloc_seisPSVfwi(ntr,ntr_glob,ns,&seisPSVfwi);
   
   /* Memory for full data seismograms */
   alloc_seisPSVfull(&seisPSV,ntr_glob);

}

/* estimate memory requirement of the variables in megabytes*/
	
switch (SEISMO){
case 1 : /* particle velocities only */
	nseismograms=2;	
	break;	
case 2 : /* pressure only */
	nseismograms=1;	
	break;	
case 3 : /* curl and div only */
	nseismograms=2;		
	break;	
case 4 : /* everything */
	nseismograms=5;		
	break;
}		

/* calculate memory requirements for PSV forward problem */
mem_fwiPSV(nseismograms,ntr,ns,fdo3,nd,buffsize,ntr_glob);

/* Define gradient formulation */
/* GRAD_FORM = 1 - stress-displacement gradients */
/* GRAD_FORM = 2 - stress-velocity gradients for decomposed impedance matrix */
GRAD_FORM = 1;

/* allocate memory for PSV forward problem */
alloc_PSV(&wavePSV,&wavePSV_PML);

/* calculate damping coefficients for CPMLs (PSV problem)*/
if(FW>0){PML_pro(wavePSV_PML.d_x, wavePSV_PML.K_x, wavePSV_PML.alpha_prime_x, wavePSV_PML.a_x, wavePSV_PML.b_x, wavePSV_PML.d_x_half, wavePSV_PML.K_x_half, wavePSV_PML.alpha_prime_x_half, wavePSV_PML.a_x_half, 
                 wavePSV_PML.b_x_half, wavePSV_PML.d_y, wavePSV_PML.K_y, wavePSV_PML.alpha_prime_y, wavePSV_PML.a_y, wavePSV_PML.b_y, wavePSV_PML.d_y_half, wavePSV_PML.K_y_half, wavePSV_PML.alpha_prime_y_half, 
                 wavePSV_PML.a_y_half, wavePSV_PML.b_y_half);
}

/* allocate memory for PSV material parameters */
alloc_matPSV(&matPSV);

/* allocate memory for PSV FWI parameters */
alloc_fwiPSV(&fwiPSV);

/* allocate memory for PSV MPI variables */
alloc_mpiPSV(&mpiPSV);

taper_coeff=  matrix(1,NY,1,NX);

/* memory for source position definition */
acq.srcpos1=fmatrix(1,8,1,1);

/* memory of L2 norm */
L2t = vector(1,4);
epst1 = vector(1,3);
	
fprintf(FP," ... memory allocation for PE %d was successfull.\n\n", MYID);

/* Holberg coefficients for FD operators*/
hc = holbergcoeff();

MPI_Barrier(MPI_COMM_WORLD);

/* Reading source positions from SOURCE_FILE */ 	
acq.srcpos=sources(&nsrc);
nsrc_glob=nsrc;


/* create model grids */
if(L){
	if (READMOD) readmod_visc_PSV(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta);
		else model(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta);
} else{
	if (READMOD) readmod_elastic_PSV(matPSV.prho,matPSV.ppi,matPSV.pu);
    		else model_elastic(matPSV.prho,matPSV.ppi,matPSV.pu);
}

/* check if the FD run will be stable and free of numerical dispersion */
if(L){
	checkfd_ssg_visc(FP,matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup,matPSV.peta,hc);
} else{
	checkfd_ssg_elastic(FP,matPSV.prho,matPSV.ppi,matPSV.pu,hc);
}
      
SHOTINC=1;

/* For RTM read only first line from FWI workflow file and set iter = 1 */
stagemax = 1;   
iter_true = 1;
iter = 1;

/* Begin of FWI-workflow */
for(nstage=1;nstage<=stagemax;nstage++){

/* read workflow input file *.inp */
FP_stage=fopen(FILEINP1,"r");
read_par_inv(FP_stage,nstage,stagemax);
/*fclose(FP_stage);*/

if((EPRECOND==1)||(EPRECOND==3)){
  Ws = matrix(1,NY,1,NX); /* total energy of the source wavefield */
  Wr = matrix(1,NY,1,NX); /* total energy of the receiver wavefield */
  We = matrix(1,NY,1,NX); /* total energy of source and receiver wavefield */
}

FC=FC_END;

if (MYID==0)
   {
   time2=MPI_Wtime();
   fprintf(FP,"\n\n\n ------------------------------------------------------------------\n");
   fprintf(FP,"\n\n\n                   Elastic Reverse Time Migration RTM \n");
   fprintf(FP,"\n\n\n ------------------------------------------------------------------\n");
   }

/* For the calculation of the material parameters between gridpoints
   they have to be averaged. For this, values lying at 0 and NX+1,
   for example, are required on the local grid. These are now copied from the
   neighbouring grids */		
if (L){
	matcopy_PSV(matPSV.prho,matPSV.ppi,matPSV.pu,matPSV.ptaus,matPSV.ptaup);
} else{
	matcopy_elastic_PSV(matPSV.prho,matPSV.ppi,matPSV.pu);
}

MPI_Barrier(MPI_COMM_WORLD);

av_mue(matPSV.pu,matPSV.puipjp,matPSV.prho);
av_rho(matPSV.prho,matPSV.prip,matPSV.prjp);
if (L) av_tau(matPSV.ptaus,matPSV.ptausipjp);


/* Preparing memory variables for update_s (viscoelastic) */
if (L) prepare_update_s_visc_PSV(matPSV.etajm,matPSV.etaip,matPSV.peta,matPSV.fipjp,matPSV.pu,matPSV.puipjp,matPSV.ppi,matPSV.prho,matPSV.ptaus,matPSV.ptaup,matPSV.ptausipjp,matPSV.f,matPSV.g,
		matPSV.bip,matPSV.bjm,matPSV.cip,matPSV.cjm,matPSV.dip,matPSV.d,matPSV.e);

/* ------------------------------------- */
/* calculate average material parameters */
/* ------------------------------------- */
Vp_avg = 0.0;
Vs_avg = 0.0;
rho_avg = 0.0;
 
for (i=1;i<=NX;i=i+IDX){
   for (j=1;j<=NY;j=j+IDY){
  
	 /* calculate average Vp, Vs */
         Vp_avg+=matPSV.ppi[j][i];
	 Vs_avg+=matPSV.pu[j][i];
	 
	 /* calculate average rho */
	 rho_avg+=matPSV.prho[j][i];

   }
}
	
/* calculate average Vp, Vs and rho of all CPUs */
Vp_sum = 0.0;
MPI_Allreduce(&Vp_avg,&Vp_sum,1,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
Vp_avg=Vp_sum;

Vs_sum = 0.0;
MPI_Allreduce(&Vs_avg,&Vs_sum,1,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
Vs_avg=Vs_sum;

rho_sum = 0.0;
MPI_Allreduce(&rho_avg,&rho_sum,1,MPI_FLOAT,MPI_SUM,MPI_COMM_WORLD);
rho_avg=rho_sum;

Vp_avg /=NXG*NYG; 
Vs_avg /=NXG*NYG; 
rho_avg /=NXG*NYG;

if(MYID==0){
   printf("Vp_avg = %.0f \t Vs_avg = %.0f \t rho_avg = %.0f \n ",Vp_avg,Vs_avg,rho_avg);	
}

C_vp = Vp_avg;
C_vs = Vs_avg;
C_rho = rho_avg;

/* ---------------------------------------------------------------------------------------------------- */
/* --------- Calculate RTM P- and S- wave image using the adjoint state method ------------------------ */
/* ---------------------------------------------------------------------------------------------------- */

L2sum = grad_obj_psv(&wavePSV, &wavePSV_PML, &matPSV, &fwiPSV, &mpiPSV, &seisPSV, &seisPSVfwi, &acq, hc, iter, nsrc, ns, ntr, ntr_glob, 
nsrc_glob, nsrc_loc, ntr_loc, nstage, We, Ws, Wr, taper_coeff, hin, DTINV_help, req_send, req_rec);

/* Interpolate missing spatial gradient values in case IDXI > 1 || IDXY > 1 */
/* ------------------------------------------------------------------------ */

if((IDXI>1)||(IDYI>1)){

   interpol(IDXI,IDYI,fwiPSV.waveconv,1);
   interpol(IDXI,IDYI,fwiPSV.waveconv_u,1);
   interpol(IDXI,IDYI,fwiPSV.waveconv_rho,1);

}

/* Preconditioning of gradients after shot summation */
precond_PSV(&fwiPSV,&acq,nsrc,ntr_glob,taper_coeff,FP_GRAV);

/* Output of RTM results */
RTM_PSV_out(&fwiPSV);

} /* End of RTM-workflow loop */

/* deallocate memory for PSV forward problem */
dealloc_PSV(&wavePSV,&wavePSV_PML);

/* deallocation of memory */
free_matrix(fwiPSV.Vp0,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.Vs0,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.Rho0,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(matPSV.prho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.prho_old,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matPSV.prip,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matPSV.prjp,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(matPSV.ppi,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.ppi_old,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matPSV.pu,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.pu_old,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(matPSV.puipjp,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_lam,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_shot,-nd+1,NY+nd,-nd+1,NX+nd);

free_matrix(mpiPSV.bufferlef_to_rig,1,NY,1,fdo3);
free_matrix(mpiPSV.bufferrig_to_lef,1,NY,1,fdo3);
free_matrix(mpiPSV.buffertop_to_bot,1,NX,1,fdo3);
free_matrix(mpiPSV.bufferbot_to_top,1,NX,1,fdo3);

free_vector(hc,0,6);

free_matrix(fwiPSV.gradg,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradp,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradg_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradp_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho_s,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_rho_shot,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradg_u,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.gradp_u,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_u,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_mu,-nd+1,NY+nd,-nd+1,NX+nd);
free_matrix(fwiPSV.waveconv_u_shot,-nd+1,NY+nd,-nd+1,NX+nd);

free_vector(fwiPSV.forward_prop_x,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_y,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_rho_x,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_rho_y,1,NY*NX*NT);
free_vector(fwiPSV.forward_prop_u,1,NY*NX*NT);

if (nsrc_loc>0){	
	free_matrix(acq.signals,1,nsrc_loc,1,NT);
	free_matrix(acq.srcpos_loc,1,8,1,nsrc_loc);
	free_matrix(acq.srcpos_loc_back,1,6,1,nsrc_loc);
}		   

 /* free memory for global source positions */
 free_matrix(acq.srcpos,1,8,1,nsrc);

 /* free memory for source position definition */
 free_matrix(acq.srcpos1,1,8,1,1);
 
 /* free memory for abort criterion */
 		
 free_vector(L2t,1,4);
 free_vector(epst1,1,3);

 if((N_STREAMER==0)||(READREC!=2)){

    if (SEISMO) free_imatrix(acq.recpos,1,3,1,ntr_glob);

    if ((ntr>0) && (SEISMO)){

            free_imatrix(acq.recpos_loc,1,3,1,ntr);
            acq.recpos_loc = NULL;
 
            switch (SEISMO){
            case 1 : /* particle velocities only */
                    free_matrix(seisPSV.sectionvx,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionvy,1,ntr,1,ns);
                    seisPSV.sectionvx=NULL;
                    seisPSV.sectionvy=NULL;
                    break;
             case 2 : /* pressure only */
                    free_matrix(seisPSV.sectionp,1,ntr,1,ns);
                    break;
             case 3 : /* curl and div only */
                    free_matrix(seisPSV.sectioncurl,1,ntr,1,ns);
                    free_matrix(seisPSV.sectiondiv,1,ntr,1,ns);
                    break;
             case 4 : /* everything */
                    free_matrix(seisPSV.sectionvx,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionvy,1,ntr,1,ns);
                    free_matrix(seisPSV.sectionp,1,ntr,1,ns);
                    free_matrix(seisPSV.sectioncurl,1,ntr,1,ns);
                    free_matrix(seisPSV.sectiondiv,1,ntr,1,ns);
                    break;

             }

    }

    free_matrix(seisPSVfwi.sectionread,1,ntr_glob,1,ns);
    free_ivector(acq.recswitch,1,ntr);
    
    if((QUELLTYPB==1)||(QUELLTYPB==3)||(QUELLTYPB==5)||(QUELLTYPB==7)){
       free_matrix(seisPSVfwi.sectionvxdata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvxdiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvxdiffold,1,ntr,1,ns);
    }

    if((QUELLTYPB==1)||(QUELLTYPB==2)||(QUELLTYPB==6)||(QUELLTYPB==7)){    
       free_matrix(seisPSVfwi.sectionvydata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvydiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionvydiffold,1,ntr,1,ns);
    }
    
    if(QUELLTYPB>=4){    
       free_matrix(seisPSVfwi.sectionpdata,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionpdiff,1,ntr,1,ns);
       free_matrix(seisPSVfwi.sectionpdiffold,1,ntr,1,ns);
    }    

 if(SEISMO){
  free_matrix(seisPSV.fulldata,1,ntr_glob,1,NT); 
 }

 if(SEISMO==1){
  free_matrix(seisPSV.fulldata_vx,1,ntr_glob,1,NT);
  free_matrix(seisPSV.fulldata_vy,1,ntr_glob,1,NT);
 }

 if(SEISMO==2){
  free_matrix(seisPSV.fulldata_p,1,ntr_glob,1,NT);
 } 
 
 if(SEISMO==3){
  free_matrix(seisPSV.fulldata_curl,1,ntr_glob,1,NT);
  free_matrix(seisPSV.fulldata_div,1,ntr_glob,1,NT);
 }

 if(SEISMO==4){
  free_matrix(seisPSV.fulldata_vx,1,ntr_glob,1,NT);
  free_matrix(seisPSV.fulldata_vy,1,ntr_glob,1,NT);
  free_matrix(seisPSV.fulldata_p,1,ntr_glob,1,NT); 
  free_matrix(seisPSV.fulldata_curl,1,ntr_glob,1,NT);
  free_matrix(seisPSV.fulldata_div,1,ntr_glob,1,NT);
 }
 
 }

 free_ivector(DTINV_help,1,NT);
 
 /* free memory for viscoelastic modeling variables */
 if (L) {
		free_matrix(matPSV.ptaus,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.ptausipjp,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.ptaup,-nd+1,NY+nd,-nd+1,NX+nd);
		free_vector(matPSV.peta,1,L);
		free_vector(matPSV.etaip,1,L);
		free_vector(matPSV.etajm,1,L);
		free_vector(matPSV.bip,1,L);
		free_vector(matPSV.bjm,1,L);
		free_vector(matPSV.cip,1,L);
		free_vector(matPSV.cjm,1,L);
		free_matrix(matPSV.f,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.g,-nd+1,NY+nd,-nd+1,NX+nd);
		free_matrix(matPSV.fipjp,-nd+1,NY+nd,-nd+1,NX+nd);
		free_f3tensor(matPSV.dip,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
		free_f3tensor(matPSV.d,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
		free_f3tensor(matPSV.e,-nd+1,NY+nd,-nd+1,NX+nd,1,L);
}
 
/* de-allocate buffer for messages */
MPI_Buffer_detach(buff_addr,&buffsize);

MPI_Barrier(MPI_COMM_WORLD);

if (MYID==0){
	fprintf(FP,"\n **Info from main (written by PE %d): \n",MYID);
	fprintf(FP," CPU time of program per PE: %li seconds.\n",clock()/CLOCKS_PER_SEC);
	time8=MPI_Wtime();
	fprintf(FP," Total real time of program: %4.2f seconds.\n",time8-time1);
	time_av_v_update=time_av_v_update/(double)NT;
	time_av_s_update=time_av_s_update/(double)NT;
	time_av_v_exchange=time_av_v_exchange/(double)NT;
	time_av_s_exchange=time_av_s_exchange/(double)NT;
	time_av_timestep=time_av_timestep/(double)NT;
	/* fprintf(FP," Average times for \n");
	fprintf(FP," velocity update:  \t %5.3f seconds  \n",time_av_v_update);
	fprintf(FP," stress update:  \t %5.3f seconds  \n",time_av_s_update);
	fprintf(FP," velocity exchange:  \t %5.3f seconds  \n",time_av_v_exchange);
	fprintf(FP," stress exchange:  \t %5.3f seconds  \n",time_av_s_exchange);
	fprintf(FP," timestep:  \t %5.3f seconds  \n",time_av_timestep);*/
		
}

fclose(FP);


}



