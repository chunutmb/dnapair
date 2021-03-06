#include "util.h"
#include "mat.h"
#include "param.h"
#include "xf.h"
#include "mf.h"



/* compute the average angle difference between the two DNA's
 * wrapping to the closest angle to `angref` */
static double aveang(double (*x)[3], double (*y)[3],
    const double *mass, int ns,
    double xc[3], double yc[3], double angref)
{
  double angx, angy, dang, wt, suma = 0, wtot = 0;
  int i;

  for ( i = 0; i < ns; i++ ) {
    double x0 = x[i][0] - xc[0];
    double x1 = x[i][1] - xc[1];
    double y0 = y[i][0] - yc[0];
    double y1 = y[i][1] - yc[1];
    angx = atan2(x1, x0);
    angy = atan2(y1, y0);
    dang = angy - angx;
    /* compute the difference between dang and angref
     * and wrap it to closest value to 0 */
    dang = fmod(dang - angref + 5 * M_PI, 2 * M_PI) - M_PI;
    wt = x0 * x0 + x1 * x1 + y0 * y0 + y1 * y1;
    if ( mass != NULL ) wt *= mass[i];
    suma += wt * dang;
    wtot += wt;
  }

  return angref + suma / wtot;
}

/* rotate and translate the first helix to the second */
static double rottrans(double (*x)[3], const double *mass,
    int ns, double *ang, double *prmsd, int verbose)
{
  double xc[2][3], rot[3][3], trans[3], rmsd;

  /* compute the center of mass of DNA 1
   * ns is the number of atoms on DNA 1 */
  calccom(x,      mass, ns, xc[0]);
  /* compute the center of mass of DNA 2
   * x + ns is the starting coordinates of DNA 2 */
  calccom(x + ns, mass, ns, xc[1]);

  /* compute the RMSD btween the two DNAs */
  rmsd = vrmsd(x, NULL, x + ns, mass, ns, 0, rot, trans);

  if ( verbose ) {
    printf("rmsd %g\n", rmsd);
    printf("trans : %10.5f %10.5f %10.5f\n\n",
          trans[0], trans[1], trans[2]);
    printf("rot   : %10.5f %10.5f %10.5f\n"
           "        %10.5f %10.5f %10.5f\n"
           "        %10.5f %10.5f %10.5f\n\n",
          rot[0][0], rot[0][1], rot[0][2],
          rot[1][0], rot[1][1], rot[1][2],
          rot[2][0], rot[2][1], rot[2][2]);
  }

  /* compute the angular difference */
  if ( ang != NULL ) {
    double dang, angref;
    /* estimate a rough value from the rotation matrix */
    angref = atan2(rot[1][0] - rot[0][1], rot[0][0] + rot[1][1]);
    /* compute the exact value, wrap to the nearest value to angref */
    dang = aveang(x, x + ns, mass, ns, xc[0], xc[1], angref);
    fprintf(stderr, "angle modified from %g(%g) to %g(%g)\n",
        angref, angref * 180 / M_PI, dang, dang * 180 / M_PI);
    if ( dang < -0.08 ) { /* make the angle positive */
      dang += 2 * M_PI;
    }
    *ang = dang;
  }

  if ( prmsd != NULL ) {
    *prmsd = rmsd;
  }

  return trans[0];
}



/* compute the mean force and torque for the list */
static void mf_dolist(xf_t *xf, char **fns, int cnt,
    const double *mass)
{
  double dis = 0, ang = 0, rmsd = 0;
  double sums[MFCNT][3] = {{0, 0, 0}}, ave[MFCNT], std[MFCNT];
  int i, j, once = 0, np = xf->np;

  /* load all files in the commandline argument */
  for ( i = 0; i < cnt; i++ ) {
    /* compute the mean force as we scan the data */
    calcmf_inplace(xf, fns[i], mass, sums);

    /* convert the sums to averages and standard deviations */
    for ( j = 0; j < MFCNT; j++ ) {
      ave[j] = sums[j][1] / sums[j][0];
      std[j] = sqrt(sums[j][2] / sums[j][0] - ave[j] * ave[j]);
    }

    if ( !once ) {
      /* for the first file in the list,
       * compare the geometry of the two helices */
      dis = rottrans(xf->x, mass, np / 2, &ang, &rmsd, 0);
      once = 1;
    }
    printf("dis %g, ang %g/%g, rmsd %g | f %g %g %g | torq %g %g %g | symmtorq %g %g %g\n",
        dis, ang, ang * 180 / M_PI, rmsd,
        ave[0], std[0], sums[0][0],
        ave[1], std[1], sums[1][0],
        ave[2], std[2], sums[2][0]);
  }
}



/* get the file pattern of the directory
 * return the maximal number of blocks */
static int getpat(char *dir, char *head, char *tail)
{
  char fnout[FILENAME_MAX], s[FILENAME_MAX];
  char cmd[FILENAME_MAX*2], *p, *q;
  FILE *fp;
  int i, block = 0;

  tmpnam(fnout);
  sprintf(cmd, "/bin/ls %s/*%s > %s", dir, tail, fnout);
  system(cmd);
  if ( (fp = fopen(fnout, "r")) == NULL ) {
    fprintf(stderr, "cannot open %s for the ls result\n", fnout);
    return -1;
  }
  if ( fgets(s, sizeof s, fp) == NULL ) {
    fclose(fp);
    return -1;
  }
  p = strstr(s, "block.");
  p[0] = '\0'; /* terminate the string */
  strcpy(head, s);

  /* count the number of files */
  rewind(fp);
  while ( fgets(s, sizeof s, fp) ) {
    p = strstr(s, "block.");
    if ( p == NULL ) continue;
    p += 6; /* skip "block." */
    q = strchr(p, '.');
    if ( q == NULL ) continue;
    *q = '\0';
    i = atoi(p);
    if ( i > block ) {
      block = i;
    }
  }

  printf("block %d, head %s\n", block, head);
  fclose(fp);
  remove(fnout);
  return block;
}



/* get a list of force files under the directory */
static char **getlist(param_t *par, int *cnt)
{
  char **fns, *fn;
  char head[FILENAME_MAX] = "sys.160.rot.60.";
  char tail[FILENAME_MAX] = ".fout.dat";
  int i, blkmax;
  FILE *fp;

  /* 1. refine the data directory */
  if ( par->dir[0] == '\0' ) {
    strcpy(par->dir, ".");
  } else {
    i = strlen(par->dir) - 1;
    /* remove the trailing slash, if any */
    if ( par->dir[i] == '/' && i != 0 ) {
      par->dir[i] = '\0';
    }
  }

  /* 2. try to get the pattern of data file */
  blkmax = getpat(par->dir, head, tail);

  /* 3. construct a list of file names */
  xnew(fns, blkmax);
  *cnt = 0;
  for ( i = 0; i < blkmax; i++ ) {
    xnew(fn, FILENAME_MAX);
    sprintf(fn, "%sblock.%d%s", head, i + 1, tail);
    /* try to check if the file exists */
    if ( (fp = fopen(fn, "r")) != NULL ) {
      fclose(fp);
      fns[ (*cnt)++ ] = fn;
    } else {
      continue;
    }
  }

  return fns;
}


/* scan all force files under the directory */
static int mfscan(param_t *par, const double *mass)
{
  int np = par->np, cnt;
  xf_t *xf;
  char **fns;

  fns = getlist(par, &cnt);
  if ( fns == NULL ) {
    return -1;
  }
  xf = xf_open(np, 1);
  mf_dolist(xf, fns, cnt, mass);
  xf_close(xf);

  return 0;
}



static int do_mf(param_t *par, int argc, char **argv)
{
  xf_t *xf;
  double *mass = NULL;
  int np = par->np;

  if ( par->usemass ) { /* use the mass as the weight */
    xnew(mass, np);
    loadmass(par->fnpsf, mass, np);
    checkmass(mass, np);
  }

  if ( par->scanf )
  {
    fprintf(stderr, "scanning directory [%s]\n", par->dir);
    mfscan(par, mass);
  }
  else
  {
    xf = xf_open(np, 500);

    if ( par->nargs == 0 ) {
      /* if no argument is provided, we compute
       * the mean force for a list a single file */
      char *fns[1];
      fns[0] = par->fninp;
      mf_dolist(xf, fns, 1, mass);
    } else {
      char **fns = NULL;
      int i, cnt = 0;

      /* treat all command line arguments
       * that do not start with '-' as input files */
      xnew(fns, argc);
      for ( i = 1; i < argc; i++ ) {
        if ( argv[i][0] != '-' ) { /* not an option */
          fns[cnt++] = argv[i];
        }
      }
      mf_dolist(xf, fns, cnt, mass);
      free(fns);
    }

    xf_close(xf);
  }

  if ( par->usemass ) {
    free(mass);
  }
  return 0;
}



int main(int argc, char **argv)
{
  param_t par[1];

  param_init(par);
  param_doargs(par, argc, argv);
  do_mf(par, argc, argv);
  return 0;
}
