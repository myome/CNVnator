// Application includes
#include "HisMaker.hh"
#include "Genotyper.hh"
#include "Genome.hh"
#include "Interval.hh"
#include "FastaParser.hh"
#include "VcfParser.hh"

#include "Math/Math.h"
#include "Math/SpecFuncMathCore.h"
#include <limits>
#include <iomanip>

#ifdef USE_YEPPP
#include <yepCore.h>
#include <yepMath.h>
#include <yepLibrary.h>
#endif

static const int N_CHROM_MAX = 20000;

double my_gaus(double *x_arr,double *par)
{
  double con   = par[0];
  double mean  = par[1];
  double sigma = par[2];
  double x     = x_arr[0];
  double d     = (x - mean)/sigma;
  return con*TMath::Exp(-0.5*d*d);
}

void normalize(TH1 *his,double max = 1,double min = -1,double norm = -1)
{
  int nbins = his->GetNbinsX(),n = 0;
  double sum = 0;
  if (max >= 0 && max <= 1) {
    int max_bin = int(max*nbins + 0.5);
    for (int b = 1;b <= max_bin;b++) {
      sum += his->GetBinContent(b);
      n++;
    }
  }
  if (min >= 0 && min <= 1) {
    int min_bin = int(min*nbins + 0.5);
    for (int b = min_bin;b <= nbins;b++) {
      sum += his->GetBinContent(b);
      n++;
    }
  }
  if (sum > 0) {
    if (norm > 0) his->Scale(norm*n/sum);
    else          his->Scale(1./sum);
  }
}

HisMaker::HisMaker(string rootFile,Genome *genome) :
  root_file_name(rootFile),
  io(rootFile),
  inv_vals(NULL),sqrt_vals(NULL),tfuncs(NULL),
  gen_his_signal(NULL),
  gen_his_distr(NULL),
  gen_his_distr_all(NULL),
  canv_view(NULL),
  refGenome_(genome)
{}

HisMaker::HisMaker(string rootFile,int binSize,bool useGCcorr,
		   Genome *genome): root_file_name(rootFile),
            io(rootFile),
				    chromosome_len(1),
				    gen_his_signal(NULL),
				    gen_his_distr(NULL),
				    gen_his_distr_all(NULL),
				    _mean(0),    _sigma(0),
				    _mean_all(0),_sigma_all(0),
				    canv_view(NULL),
				    refGenome_(genome)
{
  if (binSize <= 0) {
    cerr<<"Bin size "<<binSize<<" is not valid."<<endl;
    return;
  }
  
  bin_size     = binSize;
  inv_bin_size = 1./bin_size;
  
  dir_name = getDirName(bin_size);
  
  // Statistics on RD
  rd_gc_name       = "rd_gc_";       rd_gc_name       += binSize;
  rd_gc_xy_name    = "rd_gc_xy_";    rd_gc_xy_name    += binSize;
  rd_gc_GC_name    = "rd_gc_GC_";    rd_gc_GC_name    += binSize;
  rd_gc_xy_GC_name = "rd_gc_xy_GC_"; rd_gc_xy_GC_name += binSize;
  
  // Statistics on partitioning
  TString suff = "";
  if (useGCcorr) suff = "_GC";
  rd_level       = new TH1D("rd_level" + suff,"RD level",401,-0.5,400.5);
  rd_level_merge = new TH1D("rd_level_merge" + suff,"RD level after merge",
			    401,-0.5,400.5);
  frag_len = new TH1D("frag_len" + suff,"Partitioning fragment length",
		      1000,0.5,1000.5);
  dl       = new TH1D("dl" + suff, "Delta RD level",401,-0.5,400.5);
  dl2      = new TH2D("dl2" + suff,"Delta RD level",
		      601,-300.5,300.5,601,-300.5,300.5);
  
  // Precalculating inverse
  inv_vals = new double[N_INV];
  for (int i = 0;i < N_INV;i++) inv_vals[i] = 0;
  
  // recalculating sqrt
  sqrt_vals = new double[N_SQRT];
  for (int i = 0;i < N_SQRT;i++) sqrt_vals[i] = 0;
  
  // Allocating t-cumulative functions
  tfuncs = new TF1*[N_FUNC];
  for (int i = 0;i < N_FUNC;i++) tfuncs[i] = NULL;
}

HisMaker::~HisMaker()
{
  delete[] inv_vals;
  delete[] sqrt_vals;
  delete[] tfuncs;
}

TH1* HisMaker::getHistogram(TString name,TString alt_name)
{
  io.file.cd();
  TDirectory *d = NULL;
  d = (TDirectory*)io.file.Get(dir_name);
  if (!d) {
    cerr<<"Can't find directory '"<<dir_name<<"'."<<endl;
    return NULL;
  }
  d->cd();
  TH1 *his = (TH1*)d->Get(name);
  if (!his) {
    his = (TH1*)d->Get(alt_name);
    if (!his) return NULL;
  }
  gROOT->cd();
  TH1 *ret = (TH1*)his->Clone(his->GetName());
  return ret;
}

TH1* HisMaker::getHistogram(TString root_file,TString dir,
			    TString name,TString alt_name)
{
  TFile file(root_file);
  if (file.IsZombie()) {
    cerr<<"Can't open file '"<<root_file<<"'."<<endl;
    return NULL;
  }
  TDirectory *d = NULL;
  if (dir.Length() > 0) {
    d = (TDirectory*)file.Get(dir);
    if (!d) {
      cerr<<"Can't find directory '"<<dir<<"'."<<endl;
      return NULL;
    }
    d->cd();
  } else d = &file;
  TH1 *his = (TH1*)d->Get(name);
  if (!his) {
    his = (TH1*)d->Get(alt_name);
    if (!his) return NULL;
  }

  gROOT->cd();
  TH1 *ret = (TH1*)his->Clone(his->GetName());
  
  file.Close();
  
  return ret;
}

bool HisMaker::writeH(bool useDir,
		      TH1 *his1,TH1 *his2,TH1 *his3,
		      TH1 *his4,TH1 *his5,TH1 *his6)
{
  io.file.cd();
  if (useDir) {
    TDirectory *dir = (TDirectory*)io.file.Get(dir_name);
    if (!dir) {
      cout<<"Making directory "<<dir_name<<" ..."<<endl;
      dir = io.file.mkdir(dir_name);
      dir->Write(dir_name);
    }
    if (!dir) {
      cerr<<"Can't find/create directory '"<<dir_name<<"'."<<endl;
      return false;
    }
    dir->cd();
  }
  
  if (his1) his1->Write(his1->GetName(),TObject::kOverwrite);
  if (his2) his2->Write(his2->GetName(),TObject::kOverwrite);
  if (his3) his3->Write(his3->GetName(),TObject::kOverwrite);
  if (his4) his4->Write(his4->GetName(),TObject::kOverwrite);
  if (his5) his5->Write(his5->GetName(),TObject::kOverwrite);
  if (his6) his6->Write(his6->GetName(),TObject::kOverwrite);

  return true;
}

double HisMaker::getInverse(int n)
{
  if (n <= 0) return 0;
  if (n >= N_INV) return 1./n;
  if (inv_vals[n] == 0) inv_vals[n] = 1./n;
  return inv_vals[n];
}

double HisMaker::getSqrt(int n)
{
  if (n <= 0) return 0;
  if (n >= N_SQRT) return TMath::Sqrt(n);
  if (sqrt_vals[n] == 0) sqrt_vals[n] = TMath::Sqrt(n);
  return sqrt_vals[n];
}

TF1 *HisMaker::getTFunction(int n)
{
  if (n < 1) return NULL;
  if (n >= N_FUNC || tfuncs[n] == NULL) {
    TString desc = "ROOT::Math::tdistribution_cdf(x,";
    desc += n; desc += ")";
    TString name = "tcum_";name += n;
    TF1 *f = new TF1(name.Data(),desc.Data(),-100,100);
    if (n < N_FUNC) tfuncs[n] = f;
    return f;
  }
  return tfuncs[n];
}

void HisMaker::getAverageVariance(double *rd,int start,int stop,
				  double &average,double &variance,int &n)
{
  average = variance = 0;
  n = 0;
  for (int b = start;b <= stop;b++) {
    average  += rd[b];
    variance += rd[b]*rd[b];
    n++;
  }
  double over_n = 1./n;
  average *= over_n;
  variance = variance*over_n - average*average;
}

double HisMaker::testRegion(double value,double m,double s,int n)
{
  if (s == 0) s = 1;
  
  TF1 *cum_t = getTFunction(n - 1);
  if (cum_t == NULL) {
    cerr<<"4: Can't find proper t-function."<<endl;
    return 1;
  }
  double x = (value - m)/sqrt(s)*getSqrt(n);
  double p = cum_t->Eval(x); if (x > 0) p = 1 - p;
  return p;
}

double HisMaker::testTwoRegions(double m1,double s1,int n1,
				double m2,double s2,int n2,
				double scale)
{
  if (s1 == 0) s1 = 1;
  if (s2 == 0) s2 = 1;
  double tmp1 = s1*getInverse(n1),tmp2 = s2*getInverse(n2);
  double s = TMath::Sqrt(tmp1 + tmp2);
  double t = (m1 - m2)/s;
  double tmp = (tmp1 + tmp2)*(tmp1 + tmp2)*(n1 - 1)*(n2 - 1);
  tmp /= tmp1*tmp1*(n2 - 1) + tmp2*tmp2*(n1 - 1);
  int ndf = int(tmp + 0.5);
  
  TF1* cum_t = getTFunction(ndf);
  if (cum_t == NULL) {
    cerr<<"2: Can't find proper t-function."<<endl;
    return 1;
  }
  double ret = cum_t->Eval(t);
  if (t > 0) ret = 1 - ret;
  
  ret *= scale*inv_bin_size*getInverse(n1 + n2);
  
  return ret;
}

double HisMaker::getEValue(double mean,double sigma,double *rd,
			   int start,int end)
{
  int n = end - start + 1;
  double aver = 0,s = 0, over_n = 1./n;
  for (int b = start;b <= end;b++) {
    aver  += rd[b];
    s += rd[b]*rd[b];
  }
  aver *= over_n;
  s = TMath::Sqrt(s*over_n - aver*aver);
  
  if (s == 0) s = sigma*TMath::Sqrt(aver/mean);
  if (s == 0) s = 1;
  
  TF1* cum = getTFunction(n - 1);
  double x = (aver - mean)*getSqrt(n)/s;
  double ret = cum->Eval(x);
  
  if (x > 0) ret = 1 - ret;
  ret *= GENOME_SIZE_NORMAL*inv_bin_size*getInverse(end - start + 1);
  
  return ret;
}

bool HisMaker::sameLevel(double l1, double l2)
{
  return (TMath::Abs(l1 - l2) < PRECISION);
}

bool HisMaker::getRegionLeft(double *level,int n_bins,int bin,
			     int &start,int &stop)
{
  if (bin < 0 || bin >= n_bins) return false;
  start = bin; stop = bin;
  while (start >= 0 && sameLevel(level[start],level[stop])) start--;
  start++;
  return true;
}

bool HisMaker::getRegionRight(double *level,int n_bins,int bin,
			      int &start,int &stop)
{
  if (bin < 0 || bin >= n_bins) return false;
  start = bin; stop = start;
  while (stop < n_bins && sameLevel(level[start],level[stop])) stop++;
  stop--;
  return true;
}

void HisMaker::view(string *files,int n_files,bool useATcorr,bool useGCcorr, bool isMale)
{
  TTimer  *timer = new TTimer("gSystem->ProcessEvents();",50,kFALSE);
  TString input = "";
  while (input != "exit" && input != "quit") {
    TString chrom = "",start = "",end = "",option = "",tmp;
    istringstream sin(input.Data());
    sin>>tmp;
    if (tmp == "execute") {
      TString obj_class,class_fun,args;
      sin>>obj_class>>class_fun>>args;
      executeROOT(obj_class,class_fun,args);
    } else if (tmp == "gview" || tmp == "all") {
      generateView(useATcorr,useGCcorr);
    } else if (parseInput(input,chrom,start,end,option)) {
      if (option == "genotype") {
	for (int i = 0;i < n_files;i++) {
	  Genotyper gen(this,files[i],bin_size);
	  gen.printGenotype(chrom.Data(),
			    start.Atoi(),end.Atoi(),useATcorr,useGCcorr, isMale);
	}
      } else if (option == "print") {
	int s = start.Atoi(), e = end.Atoi();
	printRegion(chrom,s,e,useATcorr,useGCcorr);
      } else if (option == "baf") {
        int s = start.Atoi(), e = end.Atoi();
        generateViewBAF(chrom,s,e,useATcorr,useGCcorr);
      } else {
	int s = start.Atoi(), e = end.Atoi();
	if (option.IsDigit())
	  generateView(chrom,s,e,useATcorr,useGCcorr,files,option.Atoi());
	else
	  generateView(chrom,s,e,useATcorr,useGCcorr,files);
      }
    }
    timer->TurnOn();
    timer->Reset();
    input = Getline(">");
    input.ReplaceAll("\n","\0");
    input = input.Remove(TString::kBoth,' ');
    timer->TurnOff();
  }

  delete timer;
  delete canv_view;
  exit(0);
}

void interateOverPrimitives(TList *list)
{
  
}

void HisMaker::executeROOT(TString obj_class,TString class_fun,TString args)
{
  if (!canv_view) return;

  TIter i1(canv_view->GetListOfPrimitives());
  while (TPad *pad = (TPad*)i1.Next()) {
    pad->cd();
    TIter i2(pad->GetListOfPrimitives());
    while (TObject *obj = i2.Next())
      if (obj->ClassName() == obj_class) obj->Execute(class_fun,args);
    if (pad->ClassName() == obj_class) pad->Execute(class_fun,args);
    pad->Update();
  }
  if (canv_view->ClassName() == obj_class) canv_view->Execute(class_fun,args);
  canv_view->cd();
  canv_view->Update();
}

void HisMaker::printRegion(TString chrom,int start,int end,
			   bool useATcorr,bool useGCcorr)
{
  TString alt_chr               = Genome::canonicalChromName(chrom.Data());
  if (alt_chr == chrom) alt_chr = Genome::extendedChromName(chrom.Data());
  TString name           = getRawSignalName(chrom,bin_size);
  TString alt_name       = getRawSignalName(alt_chr,bin_size);
  TString name_his       = getSignalName(chrom,bin_size,false,false);
  TString alt_name_his   = getSignalName(alt_chr,bin_size,false,false);
  TString name_corr      = getSignalName(chrom,bin_size,useATcorr,useGCcorr);
  TString alt_name_corr  = getSignalName(alt_chr,bin_size,useATcorr,useGCcorr);
  TString name_partition = getPartitionName(chrom,bin_size,
					    useATcorr,useGCcorr);
  TString alt_name_part  = getPartitionName(alt_chr,bin_size,
					    useATcorr,useGCcorr);
  TString name_merge     = name_partition + "_merge";
  TString alt_name_merge = alt_name_part  + "_merge";
  TString dir = getDirName(bin_size);
  TH1 *raw    = getHistogram(root_file_name,dir,name,          alt_name);
  TH1 *his    = getHistogram(root_file_name,dir,name_his,      alt_name_his);
  TH1 *hisc   = getHistogram(root_file_name,dir,name_corr,     alt_name_corr);
  TH1 *hisp   = getHistogram(root_file_name,dir,name_partition,alt_name_part);
  TH1 *hism   = getHistogram(root_file_name,dir,name_merge,    alt_name_merge);
  TH1 *his_gc = getHistogram(getGCName(chrom,bin_size));
  if (!his_gc) his_gc = getHistogram(getGCName(alt_chr,bin_size));
  if (his)
    for (int i = 1;i <= his->GetNbinsX();i++) {
      int s = his->GetBinLowEdge(i);
      int e = s + his->GetBinWidth(i);
      if ((s >= start || e >= start) && (e <= end || s <= end)) {
	cout<<s<<"\t";
	if (his)  cout<<his->GetBinContent(i);  else cout<<-1;
	cout<<"\t";
	if (hisc) cout<<hisc->GetBinContent(i); else cout<<-1;
	cout<<"\t";
	if (hisp) cout<<hisp->GetBinContent(i); else cout<<-1;
	cout<<"\t";
	if (hism) cout<<hism->GetBinContent(i); else cout<<-1;
	cout<<"\t";
	if (his_gc) cout<<his_gc->GetBinContent(i); else cout<<-1;
	cout<<endl;
      }
    }
  return;
}

void HisMaker::generateView(bool useATcorr,bool useGCcorr) // Global view
{
  if (canv_view) {
    delete canv_view;
    canv_view = NULL;
  }
  if (!canv_view) {
    TStyle *st = new TStyle("st","st");
    st->SetOptStat(false);  // No box with statistics
    st->SetOptTitle(false); // No box with title
    gROOT->SetStyle("st"); 
    canv_view = new TCanvas("canv","canv",1400,850);
    canv_view->SetFillColor(kWhite);
    canv_view->SetBorderMode(0); // No borders
  }
  canv_view->Divide(6,4);

  double mean,sigma;
  TH1 *rd_his = getHistogram(getDistrName("chr1",bin_size,
					  useATcorr,useGCcorr));
  getMeanSigma(rd_his,mean,sigma);

  TString chroms[24] = {"chr1","chr2","chr3","chr4","chr5","chr6","chr7",
			"chr8","chr9","chr10","chr11","chr12","chr13","chr14",
			"chr15","chr16","chr17","chr18","chr19","chr20",
			"chr21","chr22","chrX","chrY"};
  
  for (int i = 0;i < 24;i++) {
    TString chrom     = chroms[i];
    TString file_name = root_file_name;
    TString alt_chr               = Genome::canonicalChromName(chrom.Data());
    if (alt_chr == chrom) alt_chr = Genome::extendedChromName(chrom.Data());
    TString name           = getRawSignalName(chrom,bin_size);
    TString alt_name       = getRawSignalName(alt_chr,bin_size);
    TString name_his       = getSignalName(chrom,bin_size,false,false);
    TString alt_name_his   = getSignalName(alt_chr,bin_size,false,false);
    TString name_corr      = getSignalName(chrom,bin_size,useATcorr,useGCcorr);
    TString alt_name_corr  = getSignalName(alt_chr,bin_size,useATcorr,useGCcorr);
    TString name_partition = getPartitionName(chrom,bin_size,
					      useATcorr,useGCcorr);
    TString alt_name_part  = getPartitionName(alt_chr,bin_size,
					      useATcorr,useGCcorr);
    TString name_merge     = name_partition + "_merge";
    TString alt_name_merge = alt_name_part  + "_merge";
    TString dir = getDirName(bin_size);
    TH1 *raw    = getHistogram(file_name,dir,name,          alt_name);
    TH1 *his    = getHistogram(file_name,dir,name_his,      alt_name_his);
    TH1 *hisc   = getHistogram(file_name,dir,name_corr,     alt_name_corr);
    TH1 *hisp   = getHistogram(file_name,dir,name_partition,alt_name_part);
    TH1 *hism   = getHistogram(file_name,dir,name_merge,    alt_name_merge);
    TVirtualPad *pad = canv_view->cd(i + 1);
    pad->SetFillColor(kWhite);
    pad->SetLineColor(kWhite);
    pad->SetFrameLineColor(kWhite);
    pad->SetFrameBorderMode(0);
    TString title = file_name; title.ReplaceAll(".root","");

    drawHistograms(chrom,0,250e+6,0,title,pad,raw,his,hisc,hisp,hism);
    if (hisc) {
      hisc->GetYaxis()->SetRangeUser(0,2*mean);
      hisc->SetLineWidth(1);
    }
    if (raw)  raw->SetLineWidth(1);
    if (his)  his->SetLineWidth(1);
    if (hisc) hisc->SetLineWidth(1);
    if (hisp) hisp->SetLineWidth(1);
    if (hism) hism->SetLineWidth(1);

    if (!his || !hisc || !hisp || !hism) {
      cout<<"For file '"<<file_name<<"'."<<endl;
      if (!his) 
	cout<<"Can't find RD histogram for '"<<chrom<<"'."<<endl;
      else if (!hisc)
	cout<<"Can't find corrected RD histogram for '"<<chrom<<"'."<<endl;
      else if (!hisp)
	cout<<"Can't find partitioning histogram for '"<<chrom<<"'."<<endl;
      else if (!hism)
	cout<<"Can't find merging histogram for '"<<chrom<<"'."<<endl;
    }
  }
  canv_view->cd(0);
  canv_view->Update();
}

void HisMaker::generateView(TString chrom,int start,int end,
			    bool useATcorr,bool useGCcorr,
			    string *files,int win)
{
  if (win <= 0) {
    win = 3*(end - start + 1);
    if (win < 10000) win = 10000;
  }

  int n_files = 1;
  if (files != NULL) {
    n_files = 0;
    while (n_files < 16 && files[n_files].length() > 0)
      n_files++;
  }
  
  if (!canv_view) {
    TStyle *st = new TStyle("st","st");
    st->SetOptStat(false);  // No box with statistics
    st->SetOptTitle(false); // No box with title
    gROOT->SetStyle("st"); 
    canv_view = new TCanvas("canv","canv",1400,850);
    canv_view->SetFillColor(kWhite);
    canv_view->SetBorderMode(0); // No borders
    if (n_files == 2)      canv_view->Divide(1,2);
    else if (n_files == 3) canv_view->Divide(1,3);
    //else if (n_files == 4) canv_view->Divide(2,2);
    else if (n_files == 4) canv_view->Divide(1,4);
    else if (n_files == 5 ||
	     n_files == 6) canv_view->Divide(2,3);
    else if (n_files == 8) canv_view->Divide(2,4);
    else if (n_files == 7 ||
	     n_files == 9) canv_view->Divide(3,3);
    else if (n_files >  9) canv_view->Divide(4,4);
  }
  
  TString title = chrom; title += ":";
  title += start; title += "-";
  title += end;
  canv_view->SetTitle(title);
  
  TString alt_chr               = Genome::canonicalChromName(chrom.Data());
  if (alt_chr == chrom) alt_chr = Genome::extendedChromName(chrom.Data());
  TString name           = getRawSignalName(chrom,bin_size);
  TString alt_name       = getRawSignalName(alt_chr,bin_size);
  TString name_his       = getSignalName(chrom,bin_size,false,false);
  TString alt_name_his   = getSignalName(alt_chr,bin_size,false,false);
  TString name_corr      = getSignalName(chrom,bin_size,useATcorr,useGCcorr);
  TString alt_name_corr  = getSignalName(alt_chr,bin_size,useATcorr,useGCcorr);
  TString name_partition = getPartitionName(chrom,bin_size,
					    useATcorr,useGCcorr);
  TString alt_name_part  = getPartitionName(alt_chr,bin_size,
					    useATcorr,useGCcorr);
  TString name_merge     = name_partition + "_merge";
  TString alt_name_merge = alt_name_part  + "_merge";
  for (int i = 0;i < n_files;i++) {
    TString file_name = root_file_name;
    if (files) file_name = files[i];
    TString dir = getDirName(bin_size);
    TH1 *raw    = getHistogram(file_name,dir,name,          alt_name);
    TH1 *his    = getHistogram(file_name,dir,name_his,      alt_name_his);
    TH1 *hisc   = getHistogram(file_name,dir,name_corr,     alt_name_corr);
    TH1 *hisp   = getHistogram(file_name,dir,name_partition,alt_name_part);
    TH1 *hism   = getHistogram(file_name,dir,name_merge,    alt_name_merge);
    TVirtualPad *pad = canv_view->cd(i + 1);
    pad->SetFillColor(kWhite);
    pad->SetLineColor(kWhite);
    pad->SetFrameLineColor(kWhite);
    pad->SetFrameBorderMode(0);
    TString title = file_name; title.ReplaceAll(".root","");
    //if (his)
      drawHistograms(chrom,start,end,win,title,pad,raw,his,hisc,hisp,hism);
    if (!his || !hisc || !hisp || !hism) {
      cout<<"For file '"<<file_name<<"'."<<endl;
      if (!his) 
	cout<<"Can't find RD histogram for '"<<chrom<<"'."<<endl;
      else if (!hisc)
	cout<<"Can't find corrected RD histogram for '"<<chrom<<"'."<<endl;
      else if (!hisp)
	cout<<"Can't find partitioning histogram for '"<<chrom<<"'."<<endl;
      else if (!hism)
	cout<<"Can't find merging histogram for '"<<chrom<<"'."<<endl;
    }
  }
  canv_view->cd(0);
  canv_view->Update();
}

void HisMaker::generateViewBAF(TString chrom,int start,int end,
                            bool useATcorr,bool useGCcorr)
{
  int win = 1*(end - start + 1);
  if (win < 10000) win = 10000;
  
  if (canv_view) delete canv_view;
  TStyle *st = new TStyle("st","st");
  st->SetOptStat(false);  // No box with statistics
  st->SetOptTitle(false); // No box with title
  gROOT->SetStyle("st");
  canv_view = new TCanvas("canv","canv",900,900);
  canv_view->SetFillColor(kWhite);
  canv_view->SetBorderMode(0); // No borders
  canv_view->Divide(1,2);
  
  TString title = chrom; title += ":";
  title += start; title += "-";
  title += end;
  canv_view->SetTitle(title);
  
  TString alt_chr               = Genome::canonicalChromName(chrom.Data());
  if (alt_chr == chrom) alt_chr = Genome::extendedChromName(chrom.Data());
  TString name           = getRawSignalName(chrom,bin_size);
  TString alt_name       = getRawSignalName(alt_chr,bin_size);
  TString name_his       = getSignalName(chrom,bin_size,false,false);
  TString alt_name_his   = getSignalName(alt_chr,bin_size,false,false);
  TString name_corr      = getSignalName(chrom,bin_size,useATcorr,useGCcorr);
  TString alt_name_corr  = getSignalName(alt_chr,bin_size,useATcorr,useGCcorr);
  TString name_partition = getPartitionName(chrom,bin_size,
                                            useATcorr,useGCcorr);
  TString alt_name_part  = getPartitionName(alt_chr,bin_size,
                                            useATcorr,useGCcorr);
  TString name_merge     = name_partition + "_merge";
  TString alt_name_merge = alt_name_part  + "_merge";
  
  TString file_name = root_file_name;
  TString dir = getDirName(bin_size);
  TH1 *raw    = getHistogram(file_name,dir,name,          alt_name);
  TH1 *his    = getHistogram(file_name,dir,name_his,      alt_name_his);
  TH1 *hisc   = getHistogram(file_name,dir,name_corr,     alt_name_corr);
  TH1 *hisp   = getHistogram(file_name,dir,name_partition,alt_name_part);
  TH1 *hism   = getHistogram(file_name,dir,name_merge,    alt_name_merge);
  TVirtualPad *pad = canv_view->cd(1);
  pad->SetFillColor(kWhite);
  pad->SetLineColor(kWhite);
  pad->SetFrameLineColor(kWhite);
  pad->SetFrameBorderMode(0);
  title = file_name; title.ReplaceAll(".root","");
  drawHistograms(chrom,start,end,win,title,pad,raw,his,hisc,hisp,hism);
  if (hisc) {
    double mean,sigma;
    TH1 *rd_his = getHistogram(getDistrName("chr1",bin_size,
                                            useATcorr,useGCcorr));
    getMeanSigma(rd_his,mean,sigma);
    hisc->GetYaxis()->SetRangeUser(0,2*mean);
    hisc->SetLineWidth(1);
  }
  pad = canv_view->cd(2);
  pad->SetFillColor(kWhite);
  pad->SetLineColor(kWhite);
  pad->SetFrameLineColor(kWhite);
  pad->SetFrameBorderMode(0);
  title = "BAF";
  stringstream sn;
  sn<<"snp_"<<chrom;
  io.file.cd();
  TTree *vcftree = (TTree*)io.file.Get(sn.str().c_str());
  if (!vcftree) {
        cerr<<"Can't find VCF tree for chromosome '"<<chrom<<"' in file '"
	    <<root_file_name<<"'."<<endl;
        return;
  }
  if(vcftree) drawHistogramsBAF(chrom,start,end,win,title,pad,his,vcftree);

  delete vcftree;
  
  canv_view->cd(0);
  canv_view->Update();
}

void HisMaker::drawHistogramsBAF(TString chrom,int start,int end,
                              int win,TString title,
                              TVirtualPad *pad,
                              TH1 *main,TTree *vcftree)
{
  int s  = start - win, e  = end + win;

  main->Draw();
  main->GetXaxis()->SetRangeUser(s,e);
  main->GetXaxis()->SetTitle(chrom);
  main->GetYaxis()->SetRangeUser(0,1.05);
  main->GetYaxis()->SetTitle(title);
  main->SetLineWidth(3);
  main->SetLineWidth(0);

  vcftree->SetMarkerSize(0.2);
  vcftree->SetMarkerStyle(21);
  vcftree->SetMarkerColor(kBlue);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==1 && (flag&2)","same");
  vcftree->SetMarkerColor(kCyan);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==1 && !(flag&2)","same");
  vcftree->SetMarkerColor(kRed);
  vcftree->Draw("nref/(nref+nalt):position","_gt%4==2 && (flag&2)","same");
  vcftree->SetMarkerColor(kOrange);
  vcftree->Draw("nref/(nref+nalt):position","_gt%4==2 && !(flag&2)","same");
  vcftree->SetMarkerColor(kGreen);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==0 && (flag&2)","same");
  vcftree->SetMarkerColor(kYellow);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==0 && !(flag&2)","same");
  vcftree->SetMarkerColor(kBlack);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==3 && (flag&2)","same");
  vcftree->SetMarkerColor(kGray);
  vcftree->Draw("nalt/(nref+nalt):position","_gt%4==3 && !(flag&2)","same");
}


void HisMaker::genotype(string *files,int n_files,
			bool useATcorr,bool useGCcorr, bool isMale)
{
  Genotyper **gs = new Genotyper*[n_files];
  for (int i = 0;i < n_files;i++)
    gs[i] = new Genotyper(this,files[i],bin_size);

  TTimer  *timer = new TTimer("gSystem->ProcessEvents();",50,kFALSE);
  TString input = "";
  while (input != "exit" && input != "quit") {
    TString chrom = "",start = "",end = "",option = "";
    if (parseInput(input,chrom,start,end,option)) {
      if (option == "view") {
	generateView(chrom,start.Atoi(),end.Atoi(),useATcorr,useGCcorr,files);
      } else {
	for (int i = 0;i < n_files;i++)
	  gs[i]->printGenotype(chrom.Data(),
			       start.Atoi(),end.Atoi(),
			       useATcorr,useGCcorr, isMale);
      }
    }
    timer->TurnOn();
    timer->Reset();
    input = Getline(">");
    input.ReplaceAll("\n","\0");
    input = input.Remove(TString::kBoth,' ');
    timer->TurnOff();
  }
  for (int i = 0;i < n_files;i++) delete gs[i];
  delete[] gs;
  delete timer;
  exit(0);
}

void HisMaker::pe_for_file(string file,string *bams,int n_bams,
			   double over,double qual)
{
  static const int max_len =  2500;
  static const int max_ses = 10000;
  int ses[max_ses];
  ifstream fin(file.c_str());
  char *str = new char[max_len];
  string type(""), coor(""),tmp("");
  while (!fin.eof()) {
    fin.getline(str,max_len);
    istringstream sin(str);
    if (sin>>type && sin>>coor) {
      //cout<<"type = "<<type<<"\t"<<"coor  = "<<coor<<endl;
      int n_pe = 0;
      TString input(coor);
      if (type == "deletion")    input.Append(" del");
      if (type == "duplication") input.Append(" tdup");
      n_pe = extract_pe(input,bams,n_bams,over,qual,false,ses);
      if (n_pe == 0) cout<<str<<"\t"<<n_pe<<endl;
      else {
	int n = max_ses; if (2*n_pe < n) n = 2*n_pe;
	int s = ses[0], e = ses[1];
	for (int i = 2;i < n;i++) {
	  if (ses[i] > s) s = ses[i];
	  i++;
	  if (ses[i] < e) e = ses[i];
	}
	TString chrom(""),start(""),end(""),option(""),new_coor("");
	if (!parseInput(input,chrom,start,end,option)) { continue; }
	new_coor = chrom; new_coor += ":"; new_coor += s;
	new_coor += "-"; new_coor += e;
	cout<<type<<"\t"<<new_coor;
	//if (sin>>tmp) cout<<"\t"<<e - s;
	while (sin>>tmp) cout<<"\t"<<tmp;
	cout<<"\t"<<n_pe<<endl;
      }
    }
  }
  return;
}

void HisMaker::pe(string *bams,int n_bams,double over,double qual)
{
  TTimer  *timer = new TTimer("gSystem->ProcessEvents();",50,kFALSE);
  TString input = "";

  while (input != "exit" && input != "quit") {
    int i = 0,n = input.Length();
    //cout<<"Working on => '"<<input<<"' ..."<<endl;
    extract_pe(input,bams,n_bams,over,qual,true);
    timer->TurnOn();
    timer->Reset();
    input = Getline(">");
    input.ReplaceAll("\n","\0");
    input = input.Remove(TString::kBoth,' ');
    timer->TurnOff();
  }

  delete timer;
  exit(0);
}

int HisMaker::extract_pe(TString input,
			 string *bams,int n_bams,double over,double qual,
			 bool do_print,int *ses)
{
  int ret = 0;
  TString chrom = "",start = "",end = "",option = "";
  if (!parseInput(input,chrom,start,end,option)) { return ret; }

  int MIN_WIN = 2000,MAX_WIN = 20000;
  map<string,int> qual_hash;
  int s = start.Atoi(), e = end.Atoi(), l = e - s + 1;
  bool forDel  = option.Contains("del", TString::kIgnoreCase);
  bool forTdup = option.Contains("tdup",TString::kIgnoreCase);
  if (forDel || forTdup) {
    int win = l;
    if (win < MIN_WIN) win = MIN_WIN;
    if (win > MAX_WIN) win = MAX_WIN;
    int srange = s - win,erange = e + win;
    qual_hash.clear();
    for (int f = 0;f < n_bams;f++) {
      if (do_print) cout<<"\t"<<bams[f]<<endl;
      AliParser *parser = new AliParser(bams[f].c_str(),true);
      int chr_index = parser->scrollTo(chrom.Data(),srange);
      if (chr_index < 0) {
	chrom.ToUpper();
	if      (chrom(0,3) == "CHR")   chrom = chrom(3,2);
	else if (chrom(0,5) == "CHROM") chrom = chrom(5,2);
	chr_index = parser->scrollTo(chrom.Data(),srange);
	if (chr_index < 0) {
	  cerr<<"Nowhere to scroll."<<endl;
	  delete parser;
	  continue;
	}
      }
      
      while (parser->parseRecord()) {
	if (parser->isUnmapped() || parser->isNextUnmapped()) continue;
	int frg_len = parser->getFragmentLength();
	int rs = parser->getStart(), re = parser->getEnd();
	if (parser->getChromosomeIndex() != chr_index ||
	    rs > erange) break;
	int fs,fe;
	
	bool go_further = false;
	if (frg_len > 0)
	  if ((forDel                &&
	       !parser->isReversed() && parser->isNextReversed()) ||
	      (forTdup               &&
	       parser->isReversed()  && !parser->isNextReversed())) {
	    fs = rs;
	    fe = fs + frg_len;
	    go_further = true;
	  }
	if (frg_len < 0)
	  if ((forDel                &&
	       parser->isReversed()  && !parser->isNextReversed()) ||
	      (forTdup               &&
	       !parser->isReversed() && parser->isNextReversed())) {
	    fe = re;
	    fs = fe + frg_len;
	    go_further = true;
	  }
	if (!go_further) continue;
	int fl = fe - fs + 1;
	int smax = s; if (fs > smax) smax = fs;
	int emin = e; if (fe < emin) emin = fe;
	int o    = emin - smax + 1;
	if (o < over*l || o < over*fl) continue;
	string name = parser->getQueryName();
	map<string,int>::iterator iter = qual_hash.find(name);
	if (iter == qual_hash.end()) { // First read
	  qual_hash[name] = parser->getQuality();
	} else { // Second read
	  int other_qual = iter->second;
	  if (other_qual >= qual && parser->getQuality() >= qual) {
	    if (do_print) cout<<input<<" "
			      <<parser->getQueryName()<<" "<<fs<<" "<<fe<<" "
			      <<other_qual<<" "<<parser->getQuality()<<endl;
	    if (ses) {
	      ses[2*ret]     = fs;
	      ses[2*ret + 1] = fe;
	    }
	    ret++;
	  }
	}
      }
      
      delete parser;
    }
  }
  return ret;
}

bool HisMaker::parseInput(TString &input,TString &chrom,
			  TString &start,TString &end,TString &option)
{
  if (input.Length() <= 0) return false;

  int i = 0;
  chrom = start = end = option = "";
  while (i < input.Length() && input[i] == ' ') i++; // Space
  while (i < input.Length() && input[i] != ':' && input[i] != ' ')
    chrom += input[i++]; i++;
  while (i < input.Length() && input[i] == ' ') i++; // Space
  while (i < input.Length() && input[i] != '-' && input[i] != ' ')
    start += input[i++]; i++;
  while (i < input.Length() && input[i] == ' ') i++; // Space
  while (i < input.Length() && input[i] != ' ')
    end   += input[i++];
  while (i < input.Length() && input[i] == ' ') i++; // Space
  while (i < input.Length() && input[i] != ' ')
    option   += input[i++];

  TString s = "", e = "", o = "";
  bool ret = true;
  if (!start.IsDigit() && start.Length() > 1) {
    s = start(0,start.Length() - 1);
    char z = start[start.Length() - 1];
    if (s.IsDigit()) {
      if      (z == 'M' || z == 'm') s += "000000";
      else if (z == 'K' || z == 'k') s += "000";
      else                           s += z;
    }
  } else s = start;
  if (!s.IsDigit()) {
    cout<<"Invalid start '"<<start<<"'."<<endl;
    ret = false;
  }
  start = s;

  if (!end.IsDigit() && end.Length() > 1) {
    e = end(0,end.Length() - 1);
    char z = end[end.Length() - 1];
    if (e.IsDigit()) {
      if      (z == 'M' || z == 'm') e += "000000";
      else if (z == 'K' || z == 'k') e += "000";
      else                           e += z;
    }
  } else e = end;
  if (!e.IsDigit()) {
    cout<<"Invalid end '"<<end<<"'."<<endl;
    ret = false;
  }
  end = e;

  if (!option.IsDigit() && option.Length() > 1) {
    o = option(0,option.Length() - 1);
    char z = option[option.Length() - 1];
    if (o.IsDigit()) {
      if      (z == 'M' || z == 'm') o += "000000";
      else if (z == 'K' || z == 'k') o += "000";
      else                           o += z;
      option = o;
    }
  }

  return ret;
}

void HisMaker::drawHistograms(TString chrom,int start,int end,
			      int win,TString title,
			      TVirtualPad *pad,
			      TH1* raw,TH1 *his,TH1 *hisc,TH1 *hisp,TH1 *hism)
{
  TH1 *main = hisc;
  if (!main) main = his;
  if (!main) main = raw;
  if (!main) return;

  int s  = start - win, e  = end + win;
  int n_bins = main->GetNbinsX();
  int bs = s/bin_size - 1; if (bs < 0) bs = 1;
  int be = e/bin_size + 1; if (be > n_bins) be = n_bins;
  double max = 0;
  for (int i = bs;i <= be;i++) {
    if (raw  && raw->GetBinContent(i)  > max) max = raw->GetBinContent(i);
    if (his  && his->GetBinContent(i)  > max) max = his->GetBinContent(i);
    if (hisc && hisc->GetBinContent(i) > max) max = hisc->GetBinContent(i);
  }
  max *= 1.05;

  main->Draw();
  main->GetXaxis()->SetRangeUser(s,e);
  main->GetXaxis()->SetTitle(chrom);
  main->GetYaxis()->SetRangeUser(0,max);
  main->GetYaxis()->SetTitle(title);
  main->SetLineWidth(3);
  if (raw) {
    raw->Draw("same hist");
    raw->SetLineColor(kYellow);
  }
  if (his) {
    his->Draw("same hist");
    his->SetLineColor(kGray);
  }
  if (hisc) {
    hisc->Draw("same hist");
    hisc->SetLineColor(kBlack);
  }
  if (hisp) {
    hisp->Draw("same");
    hisp->SetLineColor(kRed);
    hisp->SetLineWidth(3);
  }
  if (hism) {
    hism->Draw("same hist");
    hism->SetLineColor(kGreen);
    hism->SetLineWidth(3);
  }
  TLine *line1 = new TLine(0,0,0,0),*line2 = new TLine(0,0,0,0);
  line1->SetX1(start); line1->SetX2(start);
  line1->SetY1(0);     line1->SetY2(max);
  line2->SetX1(end);   line2->SetX2(end);
  line2->SetY1(0);     line2->SetY2(max);
  line1->SetLineColor(kCyan);
  line2->SetLineColor(kCyan);
  line1->SetLineWidth(3);
  line2->SetLineWidth(3);
  line1->Draw(); line2->Draw();
}

bool HisMaker::adjustToEValue(double mean,double sigma,double *rd,int n_bins,
			      int &start,int &end,double eval)
{
  static const int MAX_STEPS = 1000;
  int s0 = start, e0 = end;
  int s[MAX_STEPS],e[MAX_STEPS],step_count = 0;
  s[step_count] = start;
  e[step_count] = end;
  step_count++;
  int ll_ind = 1,lr_ind = 2,rl_ind = 3,rr_ind = 4;

  double val = getEValue(mean,sigma,rd,start,end);
  while (end > start + 1 && val > eval && step_count < MAX_STEPS) {
    double best_e = 1e+10, tmp = 0;
    int best_index = 0;
    if (start - 1 >= 0 && // Left left
	(tmp = getEValue(mean,sigma,rd,start - 1,end)) < best_e) {
      best_e = tmp;
      best_index = ll_ind;
    }
    if (start + 1 < n_bins && // Left right
	(tmp = getEValue(mean,sigma,rd,start + 1,end)) < best_e) {
      best_e = tmp;
      best_index = lr_ind;
    }
    if (end - 1 >= 0 && // Right left
	(tmp = getEValue(mean,sigma,rd,start,end - 1)) < best_e) {
      best_e = tmp;
      best_index = rl_ind;
    }
    if (end + 1 < n_bins && // Right right
	(tmp = getEValue(mean,sigma,rd,start,end + 1)) < best_e) {
      best_e = tmp;
      best_index = rr_ind;
    }
    if (best_e > val) break; // Can't improve e-value
    if (best_index == ll_ind)      start -= 1;
    else if (best_index == lr_ind) start += 1;
    else if (best_index == rl_ind) end   -= 1;
    else if (best_index == rr_ind) end   += 1;
    val = best_e;

    for (int i = 0;i < step_count;i++)
      if (start == s[i] && end == e[i]) break; // Get into loop
    s[step_count] = start;
    e[step_count] = end;
    step_count++;
  }

  if (end > start && val <= eval) return true;
  return false;
}

double HisMaker::gaussianEValue(double mean,double sigma,double *rd,
				int start,int end)
{
  // Calculate by deviation from gaussian
  double max = 0,min = 1e+10,av = 0;
  int n = end - start + 1;
  for (int i = start;i <= end;i++) {
    av += rd[i];
    if (rd[i] > max) max = rd[i];
    if (rd[i] < min) min = rd[i];
  }
    
  av *= getInverse(n);

  double p = 0;
  if (av < mean) {
    double x = (max - mean)/sigma*0.707;
    p = 0.5*(1 + TMath::Erf(x));
  } else {
    double x = (min - mean)/sigma*0.707;
    p = 0.5*(1 - TMath::Erf(x));
  }
  return GENOME_SIZE_NORMAL*TMath::Power(p,n);
}

void HisMaker::callSVs(string *user_chroms,int n_chroms,
		       bool useATcorr,bool useGCcorr, bool isMale, double delta)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting calling."<<endl;
    return;
  }

  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,useATcorr,useGCcorr);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    callSVs(chr_names,n_chroms,useATcorr,useGCcorr, isMale, delta);
    return;
  }

  for (int c = 0;c < n_chroms;c++) {
    string chrom   = user_chroms[c];
    string name    = chrom;
    
    
    
    TH1 *h_unique  = getHistogram(getUSignalName(name,bin_size));
    TH1 *h_all     = getHistogram(getSignalName(name,bin_size,false,false));
    TH1 *his       = getHistogram(getSignalName(name,bin_size,
						useATcorr,useGCcorr));
    TH1 *partition = getHistogram(getPartitionName(name,bin_size,
						   useATcorr,useGCcorr));
    TH1 *rd_his    = getHistogram(getDistrName(name,bin_size,
					       useATcorr,useGCcorr));
    TH1 *rd_his_global = getHistogram(getDistrName(Genome::CHRALL,bin_size,
						   useATcorr,useGCcorr));
    if (!his || !rd_his || !partition) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    TString hname = partition->GetName();
    TH1 *merge = (TH1*)partition->Clone(hname + "_merge");
    
    double mean,sigma;
    getMeanSigma(rd_his,mean,sigma);
    
    int n_bins = partition->GetNbinsX();
    double *level = new double[n_bins],*rd = new double[n_bins];
    char   *flags = new char[n_bins];
    for (int b = 0;b < n_bins;b++) {
      rd[b]    = his->GetBinContent(b + 1);
      level[b] = partition->GetBinContent(b + 1);
      flags[b] = ' ';
    }

    double cut = mean*delta;
    if (rd_his_global) {
      double mean_global,sigma_global;
      getMeanSigma(rd_his_global,mean_global,sigma_global);
      if (Genome::isSexChrom(chrom)) {
      if (isMale) { // For male individuals
	cerr<<"Assuming male individual!"<<endl;
	cut = 2*cut;
      } else {
        cerr <<"Assuming female individual!" << endl;
      }
      }
    }

    while (mergeLevels(level,n_bins,cut)) ;

    // Initial region identification
    double min = mean - cut;
    double max = mean + cut;
    for (int b = 0;b < n_bins;b++) {
      int b0 = b;
      int bs = b;
      while (b < n_bins && level[b] < min) b++;
      int be = b - 1;
      if (be > bs && adjustToEValue(mean,sigma,rd,n_bins,bs,be,CUTOFF_REGION))
	for (int i = bs;i <= be;i++) flags[i] = 'D';
      bs = b;
      while (b < n_bins && level[b] > max) b++;
      be = b - 1;
      if (be > bs && adjustToEValue(mean,sigma,rd,n_bins,bs,be,CUTOFF_REGION))
	for (int i = bs;i <= be;i++) flags[i] = 'A';
      if (b > b0) b--;
    }
    
    // Merging with short regions
    int n_add = 1;
    //while (n_add > 0) {
    while (delta < 0.25 && n_add > 0) { // Temporary for developing the usage of AIB
      for (int b = 0;b < n_bins;b++) {
	
	if (flags[b] != ' ') continue;
	
	int s = b;
	while (b < n_bins && flags[b] == ' ') b++;
	int e = b - 1;
	
	if (e < s || s == 0 || e >= n_bins) continue;
	if (flags[s - 1] != flags[e + 1]) continue;
	if (s == e) { flags[s] = flags[s - 1]; continue; }
	
	int le = s - 1,ls = le;
	while (ls >= 0 && flags[ls] == flags[le]) ls--; ls++;
	int rs = e + 1,re = rs;
	while (re < n_bins && flags[re] == flags[rs]) re++; re--;
	
	double average,variance;
	double raverage,rvariance;
	double laverage,lvariance;
	int n,rn,ln;
	getAverageVariance(rd, s, e, average, variance, n);
	getAverageVariance(rd,rs,re,raverage,rvariance,rn);
	getAverageVariance(rd,ls,le,laverage,lvariance,ln);
	if (n > rn || n > ln) continue;
	
	if (testTwoRegions(laverage,lvariance,ln,average,variance,n,
			   GENOME_SIZE_CNV) < CUTOFF_TWO_REGIONS &&
	    testTwoRegions(raverage,rvariance,rn,average,variance,n,
			   GENOME_SIZE_CNV) < CUTOFF_TWO_REGIONS)
	  continue;
	
	for (int i = s;i <= e;i++) flags[i] = 'C';
      }
      
      n_add = 0;
      for (int i = 0;i <= n_bins;i++) 
	if (flags[i] == 'C') {
	  flags[i] = flags[i - 1];
	  n_add++;
	}
    }
    
    // Additional deletions
    for (int b = 0;b < n_bins;b++) {
      if (flags[b] != ' ') continue;
      int bs = b;
      while (b < n_bins && level[b] < min) b++;
      int be = b - 1;
      if (be > bs) {
	if (gaussianEValue(mean,sigma,rd,bs,be) < CUTOFF_REGION)
	  for (int i = bs;i <= be;i++) flags[i] = 'd';
	b--;
      }
    }
    
    // Additional duplications
    //     for (int b = 0;b < n_bins;b++) {
    //       if (flags[b] != ' ') continue;
    //       int bs = b;
    //       while (b < n_bins && level[b] > max) b++;
    //       int be = b - 1;
    //       if (be > bs) {
    // 	if (gaussianEValue(mean,sigma,rd,bs,be) < CUTOFF_REGION)
    //  	  for (int i = bs;i <= be;i++) flags[i] = 'a';
    // 	b--;
    //       }
    //     }
    
    // Filling and saving histograms
    for (int b = 0;b < n_bins;b++) {
      int b0 = b, n = 0;
      double lev = 0,tmp = level[b];
      while (b < n_bins && sameLevel(level[b],tmp)) { lev += rd[b]; n++; b++; }
      lev *= getInverse(n);
      rd_level_merge->Fill(lev);
      b--;
    }
    
    for (int b = 0;b < n_bins;b++) {
      int b0 = b, n = 0;
      char c = flags[b];
      double lev = 0;
      while (b < n_bins  && flags[b] == c) { lev += rd[b]; n++; b++; }
      lev *= getInverse(n);
      while (b0 < b) {
	level[b0++] = lev;
	merge->SetBinContent(b0,lev);
      }
      b--;
    }
    
    io.writeHistogramsToBinDir(bin_size,rd_level_merge,merge);

    // Making calls
    for (int b = 0;b < n_bins;b++) {
      char c = flags[b];
      if (c == ' ') continue;
      int bs = b;
      double cnv = 0;
      while (b < n_bins && flags[b] == c) cnv += rd[b++];
      int be = --b;
      
      if (be <= bs) continue;
      
      cnv /= (be - bs + 1)*mean;
      TString type = "???";
      if (c == 'D' || c == 'd')      type = "deletion";
      else if (c == 'A' || c == 'a') type = "duplication";
      int start  = bs*bin_size + 1;
      int end    = (be + 1)*bin_size;
      double size = end - start + 1;
      double e  = getEValue(mean,sigma,rd,bs,be);
      double e2 = gaussianEValue(mean,sigma,rd,bs,be);
      double e3 = 1,e4 = 1;
      int add = int(1000./bin_size + 0.5);
      if (bs + add < be - add) {
	e3 = getEValue(mean,sigma,rd,bs + add,be - add);
	e4 = gaussianEValue(mean,sigma,rd,bs + add,be - add);
      }
      double n_reads_all = 0,n_reads_unique = 0;
      for (int i = bs;i <= be;i++) {
	if (h_all)    n_reads_all    += h_all->GetBinContent(i);
	if (h_unique) n_reads_unique += h_unique->GetBinContent(i);
      }
      double q0 = -1;
      if (n_reads_all > 0) q0 = (n_reads_all - n_reads_unique)/n_reads_all;
      cout<<type<<"\t"<<chrom<<":"<<start<<"-"<<end<<"\t"
	  <<size<<"\t"<<cnv<<"\t"<<e<<"\t"<<e2<<"\t"
	  <<e3<<"\t"<<e4<<"\t"<<q0<<endl;
    }
    delete[] rd;
    delete[] level;
    delete[] flags;
  }
}

void HisMaker::callSVsSignal(int bin, string signal, unsigned int flags,string *user_chroms,int n_chroms,double delta)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
    <<"Aborting calling."<<endl;
    return;
  }
  
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,false,true);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    callSVsSignal(bin,signal,flags,chr_names,n_chroms,delta);
    return;
  }
  
  for (int c = 0;c < n_chroms;c++) {
    
    string chrom   = user_chroms[c];
    string name    = chrom;
    unsigned int cflags=Genome::isSexChrom(name)?flags|FLAG_SEX:flags;
    
    cout << "Call SVs for signal: " << io.signalName(chrom, bin, signal, cflags) << endl;
    cout << "with partition: " << io.signalName(chrom, bin, signal+" partition", cflags) << endl;
    
    TH1 *his     = io.getSignal(chrom, bin, signal, cflags);
    TH1 *partition = io.getSignal(chrom, bin, signal+" partition", cflags);
    if (!his || !partition) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    cout<<"Call SVs using signal for '"<<chrom <<"' with bin size of "<<bin_size<<" ..."<<endl;
    
    
    TString hname = his->GetName();
    TH1 *merge = (TH1*)his->Clone(hname + "_call");
    
    double mean,sigma;
    getMeanSigma(his,mean,sigma);
    
    int n_bins = partition->GetNbinsX();
    double *level = new double[n_bins],*rd = new double[n_bins];
    char   *flags = new char[n_bins];
    for (int b = 0;b < n_bins;b++) {
      rd[b]    = his->GetBinContent(b + 1);
      level[b] = partition->GetBinContent(b + 1);
      flags[b] = ' ';
    }
    
    double cut = mean*delta;
    
    while (mergeLevels(level,n_bins,cut)) ;
    
    // Initial region identification
    double min = mean - cut;
    double max = mean + cut;
    for (int b = 0;b < n_bins;b++) {
      int b0 = b;
      int bs = b;
      while (b < n_bins && level[b] < min) b++;
      int be = b - 1;
      if (be > bs && adjustToEValue(mean,sigma,rd,n_bins,bs,be,CUTOFF_REGION))
        for (int i = bs;i <= be;i++) flags[i] = 'D';
      bs = b;
      while (b < n_bins && level[b] > max) b++;
      be = b - 1;
      if (be > bs && adjustToEValue(mean,sigma,rd,n_bins,bs,be,CUTOFF_REGION))
        for (int i = bs;i <= be;i++) flags[i] = 'A';
      if (b > b0) b--;
    }
    
    // Merging with short regions
    int n_add = 1;
    //while (n_add > 0) {
    while (delta < 0.25 && n_add > 0) { // Temporary for developing the usage of AIB
      for (int b = 0;b < n_bins;b++) {
        
        if (flags[b] != ' ') continue;
        
        int s = b;
        while (b < n_bins && flags[b] == ' ') b++;
        int e = b - 1;
        
        if (e < s || s == 0 || e >= n_bins) continue;
        if (flags[s - 1] != flags[e + 1]) continue;
        if (s == e) { flags[s] = flags[s - 1]; continue; }
        
        int le = s - 1,ls = le;
        while (ls >= 0 && flags[ls] == flags[le]) ls--; ls++;
        int rs = e + 1,re = rs;
        while (re < n_bins && flags[re] == flags[rs]) re++; re--;
        
        double average,variance;
        double raverage,rvariance;
        double laverage,lvariance;
        int n,rn,ln;
        getAverageVariance(rd, s, e, average, variance, n);
        getAverageVariance(rd,rs,re,raverage,rvariance,rn);
        getAverageVariance(rd,ls,le,laverage,lvariance,ln);
        if (n > rn || n > ln) continue;
        
        if (testTwoRegions(laverage,lvariance,ln,average,variance,n,
                           GENOME_SIZE_CNV) < CUTOFF_TWO_REGIONS &&
            testTwoRegions(raverage,rvariance,rn,average,variance,n,
                           GENOME_SIZE_CNV) < CUTOFF_TWO_REGIONS)
          continue;
        
        for (int i = s;i <= e;i++) flags[i] = 'C';
      }
      
      n_add = 0;
      for (int i = 0;i <= n_bins;i++)
        if (flags[i] == 'C') {
          flags[i] = flags[i - 1];
          n_add++;
        }
    }
    
    // Additional deletions
    for (int b = 0;b < n_bins;b++) {
      if (flags[b] != ' ') continue;
      int bs = b;
      while (b < n_bins && level[b] < min) b++;
      int be = b - 1;
      if (be > bs) {
        if (gaussianEValue(mean,sigma,rd,bs,be) < CUTOFF_REGION)
          for (int i = bs;i <= be;i++) flags[i] = 'd';
        b--;
      }
    }
    
    
    // Filling and saving histograms
    for (int b = 0;b < n_bins;b++) {
      int b0 = b, n = 0;
      double lev = 0,tmp = level[b];
      while (b < n_bins && sameLevel(level[b],tmp)) { lev += rd[b]; n++; b++; }
      lev *= getInverse(n);
      rd_level_merge->Fill(lev);
      b--;
    }
    
    for (int b = 0;b < n_bins;b++) {
      int b0 = b, n = 0;
      char c = flags[b];
      double lev = 0;
      while (b < n_bins  && flags[b] == c) { lev += rd[b]; n++; b++; }
      lev *= getInverse(n);
      while (b0 < b) {
        level[b0++] = lev;
        merge->SetBinContent(b0,lev);
      }
      b--;
    }
    
    io.writeHistogramsToBinDir(bin_size,rd_level_merge,merge);
    
    // Making calls
    for (int b = 0;b < n_bins;b++) {
      char c = flags[b];
      if (c == ' ') continue;
      int bs = b;
      double cnv = 0;
      while (b < n_bins && flags[b] == c) cnv += rd[b++];
      int be = --b;
      
      if (be <= bs) continue;
      
      cnv /= (be - bs + 1)*mean;
      TString type = "???";
      if (c == 'D' || c == 'd')      type = "deletion";
      else if (c == 'A' || c == 'a') type = "duplication";
      int start  = bs*bin_size + 1;
      int end    = (be + 1)*bin_size;
      double size = end - start + 1;
      double e  = getEValue(mean,sigma,rd,bs,be);
      double e2 = gaussianEValue(mean,sigma,rd,bs,be);
      double e3 = 1,e4 = 1;
      int add = int(1000./bin_size + 0.5);
      if (bs + add < be - add) {
        e3 = getEValue(mean,sigma,rd,bs + add,be - add);
        e4 = gaussianEValue(mean,sigma,rd,bs + add,be - add);
      }
      double n_reads_all = 0,n_reads_unique = 0;
      double q0 = -1;
      if (n_reads_all > 0) q0 = (n_reads_all - n_reads_unique)/n_reads_all;
      cout<<type<<"\t"<<chrom<<":"<<start<<"-"<<end<<"\t"
      <<size<<"\t"<<cnv<<"\t"<<e<<"\t"<<e2<<"\t"
      <<e3<<"\t"<<e4<<"\t"<<q0<<endl;
    }
    delete[] rd;
    delete[] level;
    delete[] flags;
  }
}


void HisMaker::getMeanSigma(TH1 *his,double &mean,double &sigma,bool dofit)
{

  mean  = his->GetMean();
  sigma = his->GetRMS();
  if (his->GetEntries() == 0) return;
  if(!dofit) return;

  int nbins  = his->GetNbinsX();
  double edge = his->GetBinLowEdge(nbins) + his->GetBinWidth(nbins);
  TF1 *fg = new TF1("my_gaus",my_gaus,0,edge,3);
  double constant = his->GetEntries()*0.4/sigma;
  fg->SetParameters(constant,mean,0.5*sigma);
  fg->SetParLimits(1,0,3*mean);
  fg->SetParLimits(2,0,3*sigma);
  his->Fit(fg,"qN");
  double min_fit = fg->GetParameter(1) - 2.0*fg->GetParameter(2);
  double max_fit = fg->GetParameter(1) + 2.0*fg->GetParameter(2);
  his->Fit(fg,"qN","",min_fit,max_fit);
  mean  = fg->GetParameter(1);
  sigma = fg->GetParameter(2);
}

double HisMaker::getMean(TH1 *his)
{
  if (his->GetMean() < his->GetRMS()) return his->GetMean();

  double mean,sigma;
  getMeanSigma(his,mean,sigma);
  return mean;
}

int HisMaker::getChromNamesWithHis(string *names,bool useATcorr,bool useGCcorr)
{
  io.file.cd();
  TDirectory *dir = (TDirectory*)io.file.Get(dir_name);
  if (!dir) {
    cerr<<"Can't find directory '"<<dir_name<<"' in file '" <<root_file_name<<"'.\n"<<endl;
    return -1;
  }

  TString v1 = "",v2 = "";
  TStringToken tok(getSignalName("CNVnator",bin_size,useATcorr,useGCcorr),
		   "CNVnator");
  if (tok.NextToken()) v1 = tok;
  if (tok.NextToken()) v2 = tok;

  int ret = 0;
  TIterator *it = dir->GetListOfKeys()->MakeIterator();
  while (TKey *key = (TKey*)it->Next()) {
    TString name = key->GetName();
    int s = 0, e = v2.Length() - 1, delta = v2.Length() - name.Length();
    while (s < v1.Length() && name[s] == v1[s]) s++;
    if (s < v1.Length()) continue;
    while (e >= 0 && e - delta >= 0 && v2[e] == name[e - delta]) e--;
    if (e >= 0) continue;
    e -= delta;
    if (s > e) continue;
    if (ret >= N_CHROM_MAX) {
      cerr<<"Too many histograms in directory  '"<<dir_name<<"' in file '"
	  <<root_file_name<<"'."<<endl
	  <<"Histogram '"<<name<<"' is ignored."<<endl;
      continue;
    }
    names[ret] = "";
    for (int i = s;i <= e;i++) names[ret] += name[i];
    ret++;
  }
  return ret;
}

void HisMaker::partition2D(string *user_chroms,int n_chroms,
			   bool skipMasked,bool useATcorr,bool useGCcorr,
			   bool exome,int range)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting partitioning."<<endl;
    return;
  }

  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,useATcorr,useGCcorr);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    partition2D(chr_names,n_chroms,skipMasked,useATcorr,useGCcorr,exome,range);
    return;
  }

  for (int c = 0;c < n_chroms;c++) {
    string chrom   = user_chroms[c];
    string name    = chrom;
    TH1 *his_rd    = getHistogram(getSignalName(name,bin_size,
					      useATcorr,useGCcorr));
    TH1 *his_aib   = getHistogram(getAIBName(name,bin_size));
    TH1 *rd_distr  = getHistogram(getDistrName(name,bin_size,
					    useATcorr,useGCcorr));

    if (!his_rd || !his_aib || !rd_distr) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    cout<<"Partitioning RD signal for '"<<chrom
	<<"' with bin size of "<<bin_size<<" ..."<<endl;

    double mean,sigma;
    getMeanSigma(rd_distr,mean,sigma);
    cout<<"Average RD per bin is "<<mean<<" +- "<<sigma<<endl;
    
    int n_bins = his_rd->GetNbinsX();
    
    TString hname = his_rd->GetName();
    TH1 *hl1 = (TH1*)his_rd->Clone(hname + "_l1");
    TH1 *hl2 = (TH1*)his_rd->Clone(hname + "_l2");
    TH1 *hl3 = (TH1*)his_rd->Clone(hname + "_l3");
    TH1 *partition =
      (TH1*)his_rd->Clone(getPartitionName(name,bin_size,useATcorr,useGCcorr));

    double *rd        = new double[n_bins];
    double *aib       = new double[n_bins];
    double *level_rd  = new double[n_bins];
    double *isig_rd   = new double[n_bins];
    double *level_aib = new double[n_bins];
    double *isig_aib  = new double[n_bins];
    double mean_4 = mean/4, sigma_2 = 4/(sigma*sigma),ms2 = mean/(sigma*sigma);
    bool   *mask  = new bool[n_bins];
    for (int b = 0;b < n_bins;b++) {
      mask[b] = false;
      rd[b]   = his_rd->GetBinContent(b + 1);
      aib[b]  = his_aib->GetBinContent(b + 1);
      isig_rd[b] = isig_aib[b] = -1;
      double err_rd  = his_rd->GetBinError(b + 1);
      double err_aib = his_aib->GetBinError(b + 1);
      if (err_rd  > 0) isig_rd[b]  = 1/(err_rd*err_rd);
      if (err_aib > 0) isig_aib[b] = 1/(err_aib*err_aib);
    }
    
    for (int bin_band = 2;bin_band <= range;bin_band++) {
      
      cout<<"Bin band is "<<bin_band<<endl;

      for (int b = 0;b < n_bins;b++) 
	if (!mask[b]) {
	  level_rd[b]  = rd[b];
	  level_aib[b] = aib[b];
	}

      if (!exome)
	for (int b = 0;b < n_bins;b++)
	  if (!mask[b])
	    isig_rd[b] = (level_rd[b] < mean_4) ? sigma_2 : ms2/level_rd[b];
      calcLevels(level_rd,isig_rd,mask,n_bins,bin_band,skipMasked,
		 level_aib,isig_aib);
      for (int b = 0;b < n_bins;b++) hl1->SetBinContent(b + 1,level_rd[b]);
      
      if (!exome)
	for (int b = 0;b < n_bins;b++)
	  if (!mask[b])
	    isig_rd[b] = (level_rd[b] < mean_4)? sigma_2: ms2/level_rd[b];
      calcLevels(level_rd,isig_rd,mask,n_bins,bin_band,skipMasked,
		 level_aib,isig_aib);
      for (int b = 0;b < n_bins;b++) hl2->SetBinContent(b + 1,level_rd[b]);
      
      if (!exome)
	for (int b = 0;b < n_bins;b++)
	  if (!mask[b])
	    isig_rd[b] = (level_rd[b] < mean_4) ? sigma_2 : ms2/level_rd[b];
      calcLevels(level_rd,isig_rd,mask,n_bins,bin_band,skipMasked,
		 level_aib,isig_aib);
      for (int b = 0;b < n_bins;b++) hl3->SetBinContent(b + 1,level_rd[b]);
      
      if (skipMasked) {
	updateMask_skip(rd,level_rd,mask,n_bins,mean,sigma);
      } else {
	updateMask(rd,level_rd,mask,n_bins,mean,sigma);
      }
      
      if (bin_band >=   8) bin_band +=  1;
      if (bin_band >=  16) bin_band +=  2;
      if (bin_band >=  32) bin_band +=  4;
      if (bin_band >=  64) bin_band +=  8;
      if (bin_band >= 128) bin_band += 16;
      if (bin_band >= 256) bin_band += 32;
      if (bin_band >= 512) bin_band += 64;
    }
    
    for (int b = 0;b < n_bins;b++) partition->SetBinContent(b + 1,level_rd[b]);
    double prev = level_rd[0],prev_delta = 0;
    int count = 1;
    for (int b = 1;b < n_bins;b++) {
      if (TMath::Abs(level_rd[b] - prev) < PRECISION) { count++; }
      else {
	rd_level->Fill(prev);
	frag_len->Fill(count);
	double delta = prev - level_rd[b];
	dl->Fill(TMath::Abs(delta));
	dl2->Fill(prev_delta,delta);
	prev = level_rd[b];
	prev_delta = delta;
	count = 1;
      }
    }
    
    delete[] rd;
    delete[] aib;
    delete[] level_rd;
    delete[] isig_rd;
    delete[] level_aib;
    delete[] isig_aib;
    delete[] mask;

    // Chromosome specific
    io.writeHistogramsToBinDir(bin_size,partition,hl1,hl2,hl3);
    delete hl1;
    delete hl2;
    delete hl3;
    delete partition;

    // General statistics
    // If many chromosomes are analyzed then sum of all will be written last
    io.writeHistogramsToBinDir(bin_size,rd_level,frag_len,dl,dl2);
  }
}

void HisMaker::partition(string *user_chroms,int n_chroms,
       bool skipMasked,bool useATcorr,bool useGCcorr,
       bool exome,int range)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
  <<"Aborting partitioning."<<endl;
    return;
  }

  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,useATcorr,useGCcorr);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    partition(chr_names,n_chroms,skipMasked,useATcorr,useGCcorr,exome,range);
    return;
  }

  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    TH1 *his     = getHistogram(getSignalName(name,bin_size,
                useATcorr,useGCcorr));
    TH1 *rd_distr = getHistogram(getDistrName(name,bin_size,
              useATcorr,useGCcorr));
    if (!his || !rd_distr) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    cout<<"Partitioning RD signal for '"<<chrom
  <<"' with bin size of "<<bin_size<<" ..."<<endl;

    double mean,sigma;
    getMeanSigma(rd_distr,mean,sigma);
    cout<<"Average RD per bin is "<<mean<<" +- "<<sigma<<endl;
    
    int n_bins = his->GetNbinsX();
    
    TString hname = his->GetName();
    TH1 *hl1 = (TH1*)his->Clone(hname + "_l1");
    TH1 *hl2 = (TH1*)his->Clone(hname + "_l2");
    TH1 *hl3 = (TH1*)his->Clone(hname + "_l3");
    TH1 *partition =
      (TH1*)his->Clone(getPartitionName(name,bin_size,useATcorr,useGCcorr));

    double *rd    = new double[n_bins];
    double *level = new double[n_bins];
    double *isig  = new double[n_bins];
    double mean_4 = mean/4, sigma_2 = 4/(sigma*sigma),ms2 = mean/(sigma*sigma);
    bool   *mask  = new bool[n_bins];
    for (int b = 0;b < n_bins;b++) {
      mask[b] = false;
      rd[b]   = his->GetBinContent(b + 1);
      double error = his->GetBinError(b + 1);
      if (error > 0) isig[b] = 1/(error*error);
      else           isig[b] = 0;
    }
    
    for (int bin_band = 2;bin_band <= range;bin_band++) {
      
      cout<<"Bin band is "<<bin_band<<endl;
      
      for (int b = 0;b < n_bins;b++)
  if (!mask[b]) level[b] = rd[b];

      if (!exome)
  for (int b = 0;b < n_bins;b++)
    if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl1->SetBinContent(b + 1,level[b]);
      
      if (!exome)
  for (int b = 0;b < n_bins;b++)
    if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl2->SetBinContent(b + 1,level[b]);
      
      if (!exome)
  for (int b = 0;b < n_bins;b++)
    if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl3->SetBinContent(b + 1,level[b]);
      
      if (skipMasked) {
  updateMask_skip(rd,level,mask,n_bins,mean,sigma);
      } else {
  updateMask(rd,level,mask,n_bins,mean,sigma);
      }
      
      if (bin_band >=   8) bin_band +=  1;
      if (bin_band >=  16) bin_band +=  2;
      if (bin_band >=  32) bin_band +=  4;
      if (bin_band >=  64) bin_band +=  8;
      if (bin_band >= 128) bin_band += 16;
      if (bin_band >= 256) bin_band += 32;
      if (bin_band >= 512) bin_band += 64;
    }
    
    for (int b = 0;b < n_bins;b++) partition->SetBinContent(b + 1,level[b]);
    double prev = level[0],prev_delta = 0;
    int count = 1;
    for (int b = 1;b < n_bins;b++) {
      if (TMath::Abs(level[b] - prev) < PRECISION) { count++; }
      else {
  rd_level->Fill(prev);
  frag_len->Fill(count);
  double delta = prev - level[b];
  dl->Fill(TMath::Abs(delta));
  dl2->Fill(prev_delta,delta);
  prev = level[b];
  prev_delta = delta;
  count = 1;
      }
    }
    
    delete[] rd;
    delete[] level;
    delete[] isig;
    delete[] mask;

    // Chromosome specific
    io.writeHistogramsToBinDir(bin_size,partition,hl1,hl2,hl3);
    delete hl1;
    delete hl2;
    delete hl3;
    delete partition;

    // General statistics
    // If many chromosomes are analyzed then sum of all will be written last
    io.writeHistogramsToBinDir(bin_size,rd_level,frag_len,dl,dl2);
  }
}

bool HisMaker::correctGC(TH1 *his,TH1 *his_gc,TH2* his_rd_gc,TH1 *his_mean)
{
  // Calculating array with average RD for GC
  int N = his_rd_gc->GetNbinsX();
  double width = 100./(N - 1),inv_width = 1./width;
  double *gc_corr = new double[N];
  calcGCcorrection(his_rd_gc,his_mean,gc_corr,N);

  int n_bins = his->GetNbinsX();
  for (int b = 1;b <= n_bins;b++) {
    double val = his->GetBinContent(b);
    double gc  = his_gc->GetBinContent(b);
    if (gc < 0) continue;
    int ind = (int)(gc*inv_width + .5);
    if (gc_corr[ind] == -1) {
      cerr<<"Zero value of GC average."<<endl;
      cerr<<"Bin "<<b<<" with center "<<his->GetBinCenter(b)
    <<" is not corrected."<<endl;
    } else {
      his->SetBinContent(b,val*gc_corr[ind]);
      his->SetBinError(b,0);
    }
  }

  delete[] gc_corr;
  return true;
}

void HisMaker::calcGCcorrection(TH2* his_rd_gc,TH1 *his_mean,
        double *corr,int n)
{
  for (int ind = 0;ind < n;ind++) {
    int bin = ind + 1;
    if (his_rd_gc->GetXaxis()->GetBinCenter(bin) > 89.99) {
      TH1 *tmp = his_rd_gc->ProjectionY("tmp",bin,n);
      while (ind < n) corr[ind++] = tmp->GetMean();
    } else {
      TH1 *tmp = his_rd_gc->ProjectionY("tmp",bin,bin);
      corr[ind] = tmp->GetMean();
    }
  }

  double global_average = getMean(his_mean);
  for (int i = 0;i < n;i++)
    if (corr[i] == 0) corr[i] = -1;
    else corr[i] = global_average/corr[i];
}

bool HisMaker::correctGCbyFragment(TH1 *his,TH1 *his_gc,
           TH2* his_rd_gc,TH1 *his_mean)
{
  double global_average = getMean(his_mean);
  if (global_average <= 0) {
    cerr<<"Can't determine global RD average."<<endl;
    return false;
  }

  TH2* his_read_frg = (TH2*)getHistogram(root_file_name,"","read_frg_len");
  if (!his_read_frg) {
    cerr<<"Can't find histogram with distribution of "
  <<"read and fragment lengths."<<endl;
    return false;
  }
  TH1 *his_frg  = his_read_frg->ProjectionY("his_frg");
  normalize(his_frg);
  
  static const int N_precalc = 40;
  double frg_fraction[N_precalc + 1];
  int nbins = his_frg->GetNbinsX();
  for (int i = 0;i <= N_precalc;i++) {
    int s = i*bin_size + 1,e = (i + 1)*bin_size;
    double val = 0;
    for (int c1 = 1;c1 <= bin_size;c1++)
      for (int c2 = s;c2 <= e;c2++) {
  int delta = c2 - c1;
  int bin = his_frg->GetBin(delta);
  if (bin >= 1 && bin <= nbins) val += his_frg->GetBinContent(bin);
      }
    val /= bin_size;
    frg_fraction[i] = val;
  }

  // Calculating array with average RD for GC
  int N = his_rd_gc->GetNbinsX();
  double width = 100./(N - 1),inv_width = 1./width;
  double *gc_average = new double[N];
  for (int ind = 0;ind < N;ind++) {
    int bin = ind + 1;
    if (his_rd_gc->GetXaxis()->GetBinCenter(bin) > 89.99) {
      TH1 *tmp = his_rd_gc->ProjectionY("tmp",bin,N);
      while (ind < N) gc_average[ind++] = tmp->GetMean();
    } else {
      TH1 *tmp = his_rd_gc->ProjectionY("tmp",bin,bin);
      gc_average[ind] = tmp->GetMean();
    }
  }

  nbins = his->GetNbinsX();
  for (int b = 1;b <= nbins;b++) {
    double val = his->GetBinContent(b),expect = 0,nb =0;
    double min1 = -1,min2 = -1;
    for (int i = 0;i <= N_precalc;i++) {
      int b1 = b - i,b2 = b + i;
      if (b1 >= 1) {
  double gc  = his_gc->GetBinContent(b1);
  if (gc < 0) continue;
  int ind = (int)(gc*inv_width + .5);
  if (gc_average[ind] == 0) continue;
  double tmp = gc_average[ind]/global_average;
  if (min1 < 0 || tmp < min1) min1 = tmp;
  if (min1 < 0) continue;
  if (b == 106130) cout<<b1<<" "<<min1<<endl;
  expect += frg_fraction[i]*min1;
  nb++;
      }
      if (b2 <= nbins) {
  double gc  = his_gc->GetBinContent(b2);
  if (gc < 0) continue;
  int ind = (int)(gc*inv_width + .5);
  double tmp = gc_average[ind]/global_average;
  if (min2 < 0 || tmp < min2) min2 = tmp;
  if (min2 < 0) continue;
  if (b == 106130) cout<<b2<<" "<<min2<<endl;
  expect += frg_fraction[i]*min2;
  nb++;
      }
    }
    expect *= 0.5*nb/(2*N_precalc + 2);
    if (b == 106130) cout<<val<<" "<<expect<<endl;
    if (expect > 0) his->SetBinContent(b,val/expect);
  }

  delete[] gc_average;
  return true;
}


void HisMaker::partitionSignal(int bin, string signal, unsigned int flags, string *user_chroms,int n_chroms,
                               bool skipMasked,bool exome,int range)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
    <<"Aborting partitioning."<<endl;
    return;
  }
  
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,false,true);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    partitionSignal(bin,signal,flags,chr_names,n_chroms,skipMasked,exome,range);
    return;
  }
  
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    
    unsigned int cflags=Genome::isSexChrom(name)?flags|FLAG_SEX:flags;

    cout << "Signal partitioning: " << io.signalName(chrom, bin, signal, cflags) << endl;
    cout << "Distribution: " << io.distributionName(bin, signal+" dist", cflags) << endl;
    TH1 *his     = io.getSignal(chrom, bin, signal, cflags);
    TH1 *rd_distr = io.getDistribution(bin, signal+" dist", cflags);
    if (!his || !rd_distr) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    cout<<"Partitioning signal for '"<<chrom
    <<"' with bin size of "<<bin_size<<" ..."<<endl;
    
    
    
    double mean,sigma;
    getMeanSigma(rd_distr,mean,sigma,false);
    cout<<"Average RD per bin is "<<mean<<" +- "<<sigma<<endl;
    
    int n_bins = his->GetNbinsX();
    
    TString hname = his->GetName();
    TH1 *hl1 = (TH1*)his->Clone(hname + "_l1");
    TH1 *hl2 = (TH1*)his->Clone(hname + "_l2");
    TH1 *hl3 = (TH1*)his->Clone(hname + "_l3");
    TH1 *partition =
    (TH1*)his->Clone(hname + "_partition");
    
    double *rd    = new double[n_bins];
    double *level = new double[n_bins];
    double *isig  = new double[n_bins];
    double mean_4 = mean/4, sigma_2 = 4/(sigma*sigma),ms2 = mean/(sigma*sigma);
    bool   *mask  = new bool[n_bins];
    for (int b = 0;b < n_bins;b++) {
      mask[b] = false;
      rd[b]   = his->GetBinContent(b + 1);
      double error = his->GetBinError(b + 1);
      if (error > 0) isig[b] = 1/(error*error);
      else           isig[b] = 0;
    }
    
    for (int b = 0;b < n_bins;b++) if(rd[b]==0) mask[b]=true;
    
    for (int bin_band = 2;bin_band <= range;bin_band++) {
      
      cout<<"Bin band is "<<bin_band<<endl;
      
      for (int b = 0;b < n_bins;b++)
      if (!mask[b]) level[b] = rd[b];
      
      if (!exome)
      for (int b = 0;b < n_bins;b++)
      if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl1->SetBinContent(b + 1,level[b]);
      
      if (!exome)
      for (int b = 0;b < n_bins;b++)
      if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl2->SetBinContent(b + 1,level[b]);
      
      if (!exome)
      for (int b = 0;b < n_bins;b++)
      if (!mask[b]) isig[b] = (level[b] < mean_4)? sigma_2: ms2/level[b];
      calcLevels(level,isig,mask,n_bins,bin_band,skipMasked);
      for (int b = 0;b < n_bins;b++) hl3->SetBinContent(b + 1,level[b]);
      
      if (skipMasked) {
        updateMask_skip(rd,level,mask,n_bins,mean,sigma);
      } else {
        updateMask(rd,level,mask,n_bins,mean,sigma);
      }
      
      for (int b = 0;b < n_bins;b++) if(rd[b]==0) mask[b]=true;

      if (bin_band >=   8) bin_band +=  1;
      if (bin_band >=  16) bin_band +=  2;
      if (bin_band >=  32) bin_band +=  4;
      if (bin_band >=  64) bin_band +=  8;
      if (bin_band >= 128) bin_band += 16;
      if (bin_band >= 256) bin_band += 32;
      if (bin_band >= 512) bin_band += 64;
    }
    
    for (int b = 0;b < n_bins;b++) partition->SetBinContent(b + 1,level[b]);
    double prev = level[0],prev_delta = 0;
    int count = 1;
    for (int b = 1;b < n_bins;b++) {
      if (TMath::Abs(level[b] - prev) < PRECISION) { count++; }
      else {
        //	rd_level->Fill(prev);
        //	frag_len->Fill(count);
        double delta = prev - level[b];
        //	dl->Fill(TMath::Abs(delta));
        //	dl2->Fill(prev_delta,delta);
        prev = level[b];
        prev_delta = delta;
        count = 1;
      }
    }
    
    delete[] rd;
    delete[] level;
    delete[] isig;
    delete[] mask;
    
    // Chromosome specific
    io.writeHistogramsToBinDir(bin_size,partition,hl1,hl2,hl3);
    delete hl1;
    delete hl2;
    delete hl3;
    delete partition;
    
    // General statistics
    // If many chromosomes are analyzed then sum of all will be written last
    //    writeHistogramsToBinDir(rd_level,frag_len,dl,dl2);
  }
}

void HisMaker::partitionSignal2D(int bin, string signal1, string signal2, unsigned int flags, string *user_chroms,int n_chroms,int range)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
    <<"Aborting partitioning."<<endl;
    return;
  }
  
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithHis(chr_names,false,true);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any histograms."<<endl;
      return;
    }
    partitionSignal2D(bin,signal1,signal2,flags,chr_names,n_chroms,range);
    return;
  }
  
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    unsigned int cflags=Genome::isSexChrom(name)?flags|FLAG_SEX:flags;
    cout << "Signal 1 partitioning: " << io.signalName(chrom, bin, signal1, cflags) << endl;
    cout << "Distribution 1: " << io.distributionName(bin, signal1+" dist", cflags) << endl;
    TH1 *his1     = io.getSignal(chrom, bin, signal1, cflags);
    TH1 *rd_distr1 = io.getDistribution(bin, signal1+" dist", cflags);
    cout << "Signal 2 partitioning: " << io.signalName(chrom, bin, signal2, cflags) << endl;
    cout << "Distribution: " << io.distributionName(bin, signal2+" dist", cflags) << endl;
    TH1 *his2     = io.getSignal(chrom, bin, signal2, cflags);
    TH1 *rd_distr2 = io.getDistribution(bin, signal2+" dist", cflags);
    if (!his1 || !rd_distr1 || !his2 || !rd_distr2) {
      cerr<<"Can't find all histograms for '"<<chrom<<"'."<<endl;
      return;
    }
    
    cout<<"Partitioning signal for '"<<chrom<<"' with bin size of "<<bin_size<<" ..."<<endl;
    
    
    
    double mean1,sigma1;
    getMeanSigma(rd_distr1,mean1,sigma1,false);
    cout<<"Average signal1 per bin is "<<mean1<<" +- "<<sigma1<<endl;
    double mean2,sigma2;
    getMeanSigma(rd_distr2,mean2,sigma2,false);
    cout<<"Average signal2 per bin is "<<mean2<<" +- "<<sigma2<<endl;

    int n_bins = his1->GetNbinsX();
    
    TString hname1 = his1->GetName();hname1+="_";hname1+=his2->GetName();
    TH1 *hl11 = (TH1*)his1->Clone(hname1 + "_l1");
    TH1 *hl12 = (TH1*)his1->Clone(hname1 + "_l2");
    TH1 *hl13 = (TH1*)his1->Clone(hname1 + "_l3");
    TH1 *partition1 = (TH1*)his1->Clone(hname1 + "_partition");
    TString hname2 = his2->GetName();hname2+="_";hname2+=his1->GetName();
    TH1 *hl21 = (TH1*)his2->Clone(hname2 + "_l1");
    TH1 *hl22 = (TH1*)his2->Clone(hname2 + "_l2");
    TH1 *hl23 = (TH1*)his2->Clone(hname2 + "_l3");
    TH1 *partition2 = (TH1*)his2->Clone(hname2 + "_partition");

    double *rd1    = new double[n_bins];
    double *level1 = new double[n_bins];
    double *isig1  = new double[n_bins];
    double mean1_4 = mean1/4, sigma1_2 = 4/(sigma1*sigma1),ms12 = mean1/(sigma1*sigma1);
    double *rd2    = new double[n_bins];
    double *level2 = new double[n_bins];
    double *isig2  = new double[n_bins];
    double mean2_4 = mean2/4, sigma2_2 = 4/(sigma2*sigma2),ms22 = mean2/(sigma2*sigma2);
    bool   *mask  = new bool[n_bins];
    bool   *mask1  = new bool[n_bins];
    bool   *mask2  = new bool[n_bins];
    for (int b = 0;b < n_bins;b++) {
      mask[b] = false;
      mask1[b] = false;
      mask2[b] = false;
      rd1[b]   = his1->GetBinContent(b + 1);
      double error1 = his1->GetBinError(b + 1);
      if (error1 > 0) isig1[b] = 1/(error1*error1);
      else           isig1[b] = 0;
      rd2[b]   = his2->GetBinContent(b + 1);
      double error2 = his2->GetBinError(b + 1);
      if (error2 > 0) isig2[b] = 1/(error2*error2);
      else           isig2[b] = 0;
    }
    
    for (int bin_band = 2;bin_band <= range;bin_band++) {
      
      cout<<"Bin band is "<<bin_band<<endl;
      
      for (int b = 0;b < n_bins;b++)
      if (!mask[b]) {
        level1[b] = rd1[b];
        level2[b] = rd2[b];
      }
      
      for (int b = 0;b < n_bins;b++) if (!mask[b]) {
        isig1[b] = (level1[b] < mean1_4)? sigma1_2: ms12/level1[b];
        isig2[b] = (level2[b] < mean2_4)? sigma2_2: ms22/level2[b];
      }
      
      calcLevels(level1,isig1,mask,n_bins,bin_band,false,level2,isig2);
      for (int b = 0;b < n_bins;b++) {
        hl11->SetBinContent(b + 1,level1[b]);
        hl21->SetBinContent(b + 1,level2[b]);
      }

      for (int b = 0;b < n_bins;b++) if (!mask[b]) {
        isig1[b] = (level1[b] < mean1_4)? sigma1_2: ms12/level1[b];
        isig2[b] = (level2[b] < mean2_4)? sigma2_2: ms22/level2[b];
      }

      calcLevels(level1,isig1,mask,n_bins,bin_band,false,level2,isig2);
      for (int b = 0;b < n_bins;b++) {
        hl12->SetBinContent(b + 1,level1[b]);
        hl22->SetBinContent(b + 1,level2[b]);
      }

      for (int b = 0;b < n_bins;b++) if (!mask[b]) {
        isig1[b] = (level1[b] < mean1_4)? sigma1_2: ms12/level1[b];
        isig2[b] = (level2[b] < mean2_4)? sigma2_2: ms22/level2[b];
      }

      calcLevels(level1,isig1,mask,n_bins,bin_band,false,level2,isig2);
      for (int b = 0;b < n_bins;b++) {
        hl13->SetBinContent(b + 1,level1[b]);
        hl23->SetBinContent(b + 1,level2[b]);
      }

      updateMask(rd1,level1,mask1,n_bins,mean1,sigma1);
      updateMask(rd2,level2,mask2,n_bins,mean2,sigma2);
      for (int b = 0;b < n_bins;b++) mask[b] = mask1[b]||mask2[b];


      if (bin_band >=   8) bin_band +=  1;
      if (bin_band >=  16) bin_band +=  2;
      if (bin_band >=  32) bin_band +=  4;
      if (bin_band >=  64) bin_band +=  8;
      if (bin_band >= 128) bin_band += 16;
      if (bin_band >= 256) bin_band += 32;
      if (bin_band >= 512) bin_band += 64;
    }
    
    for (int b = 0;b < n_bins;b++) {
      partition1->SetBinContent(b + 1,level1[b]);
      partition2->SetBinContent(b + 1,level2[b]);
    }
    
    delete[] rd1;
    delete[] level1;
    delete[] isig1;
    delete[] rd2;
    delete[] level2;
    delete[] isig2;
    delete[] mask;
    
    io.writeHistogramsToBinDir(bin_size,partition1,hl11,hl12,hl13);
    io.writeHistogramsToBinDir(bin_size,partition2,hl21,hl22,hl23);
    delete hl11;
    delete hl12;
    delete hl13;
    delete partition1;
    delete hl21;
    delete hl22;
    delete hl23;
    delete partition2;

  }
}


void HisMaker::updateMask(double *rd,double *level,bool *mask,int n_bins,
			  double mean,double sigma)
{
  for (int b = 0;b < n_bins;b++) mask[b] = false;

  int ln = 0,n = 0,rn = 0;
  double average  = 0, variance  = 0;
  double laverage = 0, lvariance = 0;
  double raverage = 0, rvariance = 0;
  double inv_mean = 1/mean;
  int start = 0, stop = -1;
  while (getRegionRight(level,n_bins,stop + 1,start,stop)) {

    ln         = n;
    laverage   = average;
    lvariance  = variance;

    n        = rn;
    average  = raverage;
    variance = rvariance;

    int rstart,rstop;
    if (!getRegionRight(level,n_bins,stop + 1,rstart,rstop)) break;
    getAverageVariance(rd,rstart,rstop,raverage,rvariance,rn);

    int lstart,lstop;
    if (!getRegionLeft(level,n_bins,start - 1,lstart,lstop)) {
      // Have no left region -- need to calculate variances
      getAverageVariance(rd,start,stop,average,variance,n);
      continue;
    }

    if (n <= 1) continue;

    if (ln <= 15 || n <= 15 || rn <= 15) {
      // Check sigma condition
      double ns = 1.8;
      //double ns = 2.0;
      double nsigma = ns*TMath::Sqrt(level[lstop]*inv_mean)*sigma;
      if (TMath::Abs(level[lstop] - level[start]) < nsigma) continue;
      nsigma = ns*TMath::Sqrt(level[rstart]*inv_mean)*sigma;
      if (TMath::Abs(level[rstart] - level[stop]) < nsigma) continue;
    } else {
      // Checking compartibility of regions
      if (testTwoRegions(laverage,lvariance,ln,average,variance,n,
			 GENOME_SIZE) > CUTOFF_TWO_REGIONS ||
	  testTwoRegions(raverage,rvariance,rn,average,variance,n,
			 GENOME_SIZE) > CUTOFF_TWO_REGIONS)
	continue;
    }
			       
    // Check that region is abnormal (deviates largerly from the mean)
    if (testRegion(mean,average,variance,n) > CUTOFF_REGION) continue;

    // Mask
    for (int b = start;b <= stop;b++) mask[b] = true;
  }
}

void HisMaker::updateMask_skip(double *rd,double *level,bool *mask,int n_bins,
			       double mean,double sigma)
{
  for (int b = 0;b < n_bins;b++) mask[b] = false;

  double inv_mean = 1/mean;
  int start = 0, stop = -1;
  while (getRegionRight(level,n_bins,stop + 1,start,stop)) {

    double average  = 0, variance  = 0;
    int n = 0;
    getAverageVariance(rd,start,stop,average,variance,n);
    if (n == 1) continue;

    int ln = 0, rn = 0;
    int rstart,rstop,lstart,lstop;
    if (!getRegionRight(level,n_bins,stop + 1,rstart,rstop)) continue;
    rn = rstop - rstart + 1;
    if (!getRegionLeft(level,n_bins,start - 1,lstart,lstop)) continue;
    ln = lstop - lstart + 1;

    double tmp = GENOME_SIZE*getInverse(n + rn + ln)*inv_bin_size;

    // Check that region is unlikely to be CNV
    if (testRegion(mean,average,variance,n) > CUTOFF_REGION) continue;

    if (ln <= 5 || n <= 5 || rn <= 5) {
      // Check sigma condition
      double ns = 1.8;
      double nsigma = ns*TMath::Sqrt(level[lstop]*inv_mean)*sigma;
      if (TMath::Abs(level[lstop] - level[start]) < nsigma) continue;
      nsigma = ns*TMath::Sqrt(level[rstart]*inv_mean)*sigma;
      if (TMath::Abs(level[rstart] - level[stop]) < nsigma) continue;
    } else {
      // Checking compartibility of regions
      if (tmp*testRegion(level[start - 1],average,variance,n) > CUTOFF_REGION)
	continue;
      if (tmp*testRegion(level[stop  + 1],average,variance,n) > CUTOFF_REGION)
	continue;
    }

    // Mask
    for (int b = start;b <= stop;b++) mask[b] = true;
  }
}

void calcLevelsInner(const double* lev_rd,const double *inv_rd,
		     const bool* mask,int n_bins,int bin_band,double* grad_b,
		     const double *lev_aib = NULL,const double *inv_aib = NULL)
{
  bool use_aib = lev_aib && inv_aib;
  
  std::vector<int>    nonMaskedIndices;
  std::vector<double> nonMaskedLevs_rd, nonMaskedLevs_aib;
  std::vector<double> nonMaskedInvs_rd, nonMaskedInvs_aib;
  nonMaskedIndices.reserve(n_bins);
  nonMaskedLevs_rd.reserve(n_bins);
  nonMaskedInvs_rd.reserve(n_bins);
  if (use_aib) {
    nonMaskedLevs_aib.reserve(n_bins);
    nonMaskedInvs_aib.reserve(n_bins);
  }

  double inv2_bin_band = 1./(bin_band*bin_band);
  int    win = 3*bin_band;
  double *exps = new double[2 * win + 1];

  for (int b = 0; b < n_bins; b++) {
    if (mask[b]) continue;
    nonMaskedIndices.push_back(b);
    nonMaskedLevs_rd.push_back(lev_rd[b]);
    nonMaskedInvs_rd.push_back(inv_rd[b]);
    if (use_aib) {
      nonMaskedLevs_aib.push_back(lev_aib[b]);
      nonMaskedInvs_aib.push_back(inv_aib[b]);
    }
  }

  for (int i = -win;i <= win;i++)
    exps[i + win] = i*exp(-0.5*i*i*inv2_bin_band);


  double *window_rd, *window_next, *window_aib;
  #pragma omp parallel private(window_rd, window_aib)
  {
    window_rd    = new double[2*(2*win + 1)];
    window_next  = &window_rd[2*win + 1];
    if (use_aib)
      window_aib = new double[2*(2*win + 1)];

    #pragma omp for schedule(static, 32)
    for (int bb = 0; bb < nonMaskedIndices.size(); bb++) {
      int b = nonMaskedIndices[bb];
      double lev_rd_b = nonMaskedLevs_rd[bb];
      double grad_b_b = grad_b[b];
      int left = std::max(0, bb - win);
      int right = std::min((int)nonMaskedIndices.size() - 1, bb + win);
      for (int ii = left; ii <= right; ii++) {
        double r   = nonMaskedLevs_rd[ii] - lev_rd_b;
        double val = -0.5*r*r;
        window_rd[ii - bb + win] = val*nonMaskedInvs_rd[bb];
	if (use_aib) {
	  window_aib[ii - bb + win] = 0;
	  if (nonMaskedInvs_aib[bb] >= 0 &&
	      nonMaskedInvs_aib[ii] >= 0) { // Negative value means undefined
	    r   = nonMaskedLevs_aib[ii] - nonMaskedLevs_aib[bb];
	    val = -0.5*r*r;
	    window_aib[ii - bb + win] = val*TMath::Sqrt(nonMaskedInvs_aib[bb]*nonMaskedInvs_aib[bb] + nonMaskedInvs_aib[ii]*nonMaskedInvs_aib[ii]);
	  }
	}
      }
#ifdef USE_YEPPP
      yepMath_Exp_V64f_V64f(window_rd + left - bb + win, window_next + left - bb + win, right - left + 1);
      double dot_product = 0;
      yepCore_DotProduct_V64fV64f_S64f(exps + left - bb + win, window_next + left - bb + win, &dot_product, right - left + 1);
      grad_b_b += dot_product;
#else
      for (int ii = left; ii <= right; ii++) {
        double to_add = exps[ii - bb + win]*exp(window_rd[ii - bb + win]);
	if (use_aib) to_add *= exp(window_aib[ii - bb + win]);
	grad_b_b += to_add;
      }
#endif
      grad_b[b] = grad_b_b;
    }

    delete [] window_rd;
    if (use_aib)
      delete [] window_aib;
  }

  delete [] exps;
}

void HisMaker::calcLevels(double *level1,double *isig1,
			  bool *mask,int n_bins,
			  int bin_band,bool skipMasked,
			  double *level2,double *isig2)
{
  double *grad_b = new double[n_bins];
  memset(grad_b,0,n_bins*sizeof(double));

  // Calculating levels
  calcLevelsInner(level1,isig1,mask,n_bins,bin_band,grad_b,
		  level2,isig2);

  for (int b = 0;b < n_bins;b++) {
    if (mask[b]) continue;
    int b_start = b;

    // Finding region by bins
    if (skipMasked) {
      while (b < n_bins && (grad_b[b] >= 0 || mask[b])) b++;
      while (b < n_bins && (grad_b[b] <  0 || mask[b])) b++;
    } else {
      while (b < n_bins && grad_b[b] >= 0 && !mask[b]) b++;
      while (b < n_bins && grad_b[b] <  0 && !mask[b]) b++;
    }
    int b_stop = --b;
    if (b_start > b_stop) {
      cerr<<"Abnormal range ("<<b_start<<", "<<b_stop<<")"<<endl;
      b = b_start;
      continue;
    }

    // Calculating level
    double nl = 0;
    double n2 = 0;
    int n = 0;
    for (int i = b_start;i <= b_stop;i++) {
      if (mask[i]) continue;
      nl += level1[i];
      if(level2) n2 += level2[i];
      n++;
    }
    if (n <= 0) {
      cerr<<"Region of length <= 0 between "<<b_start<<" and "<<b_stop<<endl;
      continue;
    }
    nl *= getInverse(n);
    n2 *= getInverse(n);
    for (int i = b_start;i <= b_stop;i++) {
      if (!mask[i]) level1[i] = nl;
      if (!mask[i]) if(level2) level2[i] = n2;
    }
  }

  delete[] grad_b;
}

bool HisMaker::mergeLevels(double *level,int n_bins,double delta)
{
  bool ret = false;

  int start1 = 0, end1 = 0, start2, end2;
  while (end1 < n_bins &&
	 TMath::Abs(level[end1] - level[start1]) < PRECISION) end1++;
  end1--; start2 = end2 = end1 + 1;
  while (end2 < n_bins &&
	 TMath::Abs(level[end2] - level[start2]) < PRECISION) end2++;
  end2--;

  while (start2 < n_bins) {
    double v1 = TMath::Abs(level[start1] - level[start2]);
    if (v1 < delta) {
      double v2 = v1 + 1, v3 = v1 + 1;
      if (start1 - 1 >= 0)
	v2 = TMath::Abs(level[start1] - level[start1 - 1]);
      if (end2 + 1 < n_bins)
	v3 = TMath::Abs(level[end2]   - level[end2 + 1]);
      if (v1 < v2 && v1 < v3) {
	ret = true;
	double nl = (end1 - start1 + 1)*level[start1];
	nl       += (end2 - start2 + 1)*level[start2];
	nl       *= getInverse(end2 - start1 + 1);
	for (int i = start1;i <= end2;i++) level[i] = nl;
	start2 = start1;
      }
    }
    start1 = start2;
    end1   = end2;
    start2 = end2 = end1 + 1;
    while (end2 < n_bins &&
	   TMath::Abs(level[end2] - level[start2]) < PRECISION) end2++;
    end2--;
  }

  return ret;
}

static const int N_smear = 40;
TH1 *h_ins_smear[N_smear + 1];
TH1 *h_read_smear[N_smear + 1];

double valley(double *x_arr,double *par)
{
  int sm = int(par[6] + 0.5);
  if (sm < 0 || sm > N_smear) sm = 0;
  TH1 *h_read = h_read_smear[sm];
  TH1 *h_ins  = h_ins_smear[sm];
  double x = x_arr[0];
  if (x < 0) x = -x;
  if (x <= par[0]) x = par[0];
  if (x <= par[2]) {
    double ret = (2 - par[1])/par[2]*x + par[1];
    int bin = int(x - 0.5);
    if (bin > 0 && bin <= h_read->GetNbinsX())
      ret += par[5]*h_read->GetBinContent(bin);
    bin = int(x - par[3] + 0.5);
    if (bin > 0 && bin <= h_ins->GetNbinsX())
      ret += par[4]*h_ins->GetBinContent(bin);
    return ret;
  } else return 2;
}


TTree *HisMaker::fitValley2ATbias(TH1 *his_read,TH1 *his_frg)
{
  TH2 *his_at = (TH2*)getHistogram(getATaggrName());
  int nbinsx  = his_read->GetNbinsX(), nbinsy = his_frg->GetNbinsY();
  
  h_ins_smear[0]  = his_frg;
  h_read_smear[0] = his_read;
  for (int s = 1;s <= N_smear;s++) {
    double denom = 0.5/s/s;
    TString hname = "read_len_smear"; hname += s;
    TH1 *his_read_smear = (TH1*)his_read->Clone(hname);
    int n_bins = his_read->GetNbinsX();
    for (int i = 1;i <= n_bins;i++) {
      double val = 0;
      for (int j = 1;j <= n_bins;j++)
        val += exp(-(i - j)*(i - j)*denom)*his_read->GetBinContent(j);
      his_read_smear->SetBinContent(i,val);
    }
    hname = "ins_len_smear"; hname += s;
    TH1 *his_frg_smear  = (TH1*)his_frg->Clone(hname);
    n_bins = his_frg->GetNbinsX();
    for (int i = 1;i <= n_bins;i++) {
      double val = 0;
      for (int j = 1;j <= n_bins;j++)
        val += exp(-(i - j)*(i - j)*denom)*his_frg->GetBinContent(j);
      his_frg_smear->SetBinContent(i,val);
    }
    normalize(his_read_smear);
    normalize(his_frg_smear);
    h_ins_smear[s]  = his_frg_smear;
    h_read_smear[s] = his_read_smear;
  }

  int len;
  double range,min,shift,middle,norm_ins,norm_read,smear,zero;
  TTree *tree_pars = new TTree("at_fit_params","Parameters");
  tree_pars->SetDirectory(0);
  tree_pars->Branch("len",      &len,      "len/I");
  tree_pars->Branch("middle",   &middle,   "middle/D");
  tree_pars->Branch("min",      &min,      "min/D");
  tree_pars->Branch("range",    &range,    "range/D");
  tree_pars->Branch("shift",    &shift,    "shift/D");
  tree_pars->Branch("norm_ins", &norm_ins, "norm_ins/D");
  tree_pars->Branch("norm_read",&norm_read,"norm_read/D");
  tree_pars->Branch("smear",    &smear,    "smear/D");
  tree_pars->Branch("zero",     &zero,     "zero/D");

  TTimer *timer = new TTimer("gSystem->ProcessEvents();",50,kFALSE);
  TH1 *hx = his_at->ProjectionX("hx");
  TF1 *func = NULL;
  int nbinsl = his_at->GetNbinsX();
  range  = his_frg->GetMean();
  for (int l = nbinsl - 1;l >= 1;l--) {
    len   = int(hx->GetBinCenter(l) + 0.5);
    TString his_name = "his_"; his_name += len;
    TH1 *his = his_at->ProjectionY(his_name,l,l);
    int zero_bin = -1,nbins = his->GetNbinsX(); 
    for (int b = 1;b <= nbins;b++) {
      double low = his->GetBinLowEdge(b);
      double up  = low + his->GetBinWidth(b);
      if (low < 0 && up > 0) {
	zero_bin = b;
	break;
      }
    }
    his->SetBinContent(zero_bin,his->GetBinContent(zero_bin)/len);
    normalize(his,0.25,0.75,2);
    min = his->GetBinContent(zero_bin);
    if (min > 2) min = 2;

    double start = his->GetBinLowEdge(1);
    double end   = his->GetBinLowEdge(nbins) + his->GetBinWidth(nbins);
    if (!func) {
      func = new TF1("func",valley,start,end,7);
      func->SetParameter(0,10);     func->SetParLimits(0,0,100);     // Middle
      func->SetParameter(1,min);    func->SetParLimits(1,0,2);       // Min
      func->SetParameter(2,range*2);func->SetParLimits(2,range,1000);// Outer
      func->SetParameter(3,0);      func->SetParLimits(3,-15,15);    // Shift
      func->SetParameter(4,45);     func->SetParLimits(4,0,100);     // I norm
      func->SetParameter(5,25);     func->SetParLimits(5,0,100);     // R norm
      func->SetParameter(6,8);      func->SetParLimits(6,0,N_smear); // Smear
      func->SetNpx(100000);
      func->SetLineColor(kGreen);
    }
    TF1* func2 = new TF1("func2",valley,start,end,7);
    func2->SetParameter(0,35);
    //double val = len*0.039 - 0.59;
    double val = len*0.032 - 0.395;
    if (val < 0) val = 0; val = 2 - val;
    func2->SetParameter(1,val);
    func2->SetParameter(2,550);
    func2->SetParameter(3,-3);
    func2->SetParameter(4,2100*(2 - val)/len);
    func2->SetParameter(5,1050*(2 - val)/len);
    func2->SetParameter(6,9.5);
    func2->SetNpx(100000);
    func2->SetLineColor(kBlue);

    //his->Draw();
    //his->GetYaxis()->SetRangeUser(0,2.5);
    his->Fit(func,"qN","",start,end);
    //func->Draw("same");
    //func2->Draw("same");

    //gPad->cd(0);
    //gPad->Update();

    middle    = func->GetParameter(0);
    min       = func->GetParameter(1);
    range     = func->GetParameter(2);
    shift     = func->GetParameter(3);
    norm_ins  = func->GetParameter(4);
    norm_read = func->GetParameter(5);
    smear     = func->GetParameter(6);
    zero      = func->Eval(0);
    tree_pars->Fill();         

//     TString input;
//     timer->TurnOn();
//     timer->Reset();
//     input = Getline(">");
//     input.ReplaceAll("\n","\0");
//     input = input.Remove(TString::kBoth,' ');
//     timer->TurnOff();
//     if (input == "exit") return NULL;
  }
  
  tree_pars->Write(tree_pars->GetName(),TObject::kOverwrite);
  return tree_pars;
}

void makeReadAndFrgHis(TH2 *his_read_frg,TH1 *his_read,TH1 *his_frg) 
{
  if (!his_read_frg) return;
    int nbinsx = his_read_frg->GetNbinsX(),nbinsy = his_read_frg->GetNbinsY();
    for (int x = 1;x <= nbinsx;x++) {
      int rl = int(his_read->GetBinCenter(x) + 0.5), half_rl = rl>>1;
      for (int y = 1;y <= nbinsy;y++) {
	double frg = his_frg->GetBinCenter(y);
	int frg_len  = int(frg - half_rl + 0.5);
	double val = his_read_frg->GetBinContent(x,y) +
	  his_frg->GetBinContent(frg_len);
	his_frg->SetBinContent(frg_len,val);
	val = his_read_frg->GetBinContent(x,y) +
	  his_read->GetBinContent(half_rl);
	his_read->SetBinContent(half_rl,val);
      }
    }
    normalize(his_frg);
    normalize(his_read);
}

void HisMaker::stat(string *user_chroms,int n_chroms,bool useATcorr)
{
  string chr_names[N_CHROM_MAX] = {""};
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
  	<<"Aborting making statistics."<<endl;
    return;
  }

  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    
    n_chroms = getChromNamesWithHis(chr_names,false,false);
    if (n_chroms < 0) return;
    if (n_chroms == 0) {
      cerr<<"Can't find any signal histograms."<<endl;
      return;
    }
    user_chroms = chr_names;
  }

  if (useATcorr) {
    TH2* his_read_frg = (TH2*)getHistogram(root_file_name,"","read_frg_len");
    TH1 *his_read = NULL,*his_frg = NULL;
    if (!his_read_frg) {
      cerr<<"Can't find histogram with distribution of "
	  <<"read and fragment lengths."<<endl;
    } else {
      his_read = his_read_frg->ProjectionX("his_read"); his_read->Reset();
      his_frg  = his_read_frg->ProjectionY("his_frg");  his_frg->Reset();
      makeReadAndFrgHis(his_read_frg,his_read,his_frg);
    }
    double RANGE = 0,NORM_FRG = 0,NORM_READ = 0,SHIFT = 0,P0 = 0,P1 = 0;
    TTree *tree_pars = fitValley2ATbias(his_read,his_frg);
    if (tree_pars) {
      double x[100],y[100];
      int len,n_gr = 0;
      double rg,min,shift,middle,norm_ins,norm_read,smear;
      tree_pars->SetBranchAddress("len",      &len);
      tree_pars->SetBranchAddress("middle",   &middle);
      tree_pars->SetBranchAddress("min",      &min);
      tree_pars->SetBranchAddress("range",    &rg);
      tree_pars->SetBranchAddress("shift",    &shift);
      tree_pars->SetBranchAddress("norm_ins", &norm_ins);
      tree_pars->SetBranchAddress("norm_read",&norm_read);
      tree_pars->SetBranchAddress("smear",    &smear);
      int n_ent = tree_pars->GetEntries();
      double n_av = 0;
      for (int e = 0;e < n_ent;e++) {
	tree_pars->GetEntry(e);
	x[n_gr] = len;
	y[n_gr] = 2 - min;
	n_gr++;
	if (len >= 40 && len <= 55) {
	  RANGE     += rg;
	  NORM_FRG  += norm_ins*len/(2 - min);
	  NORM_READ += norm_read*len/(2 - min);
	  SHIFT     += shift;
	  n_av++;
	}
      }
      
      if (n_av > 0) {
	RANGE /= n_av, NORM_FRG /= n_av, NORM_READ /= n_av, SHIFT /= n_av;
	TGraph *graph = new TGraph(n_gr,x,y);
	graph->Fit("pol1","q");
	P0 = graph->GetFunction("pol1")->GetParameter(0);
	P1 = graph->GetFunction("pol1")->GetParameter(1);
	cout<<"Pars "<<RANGE<<" "<<NORM_FRG<<" "<<NORM_READ<<" "<<SHIFT<<endl;
	cout<<P0<<" "<<P1<<endl;
      }
    }
    
    if (his_read_frg && his_read && his_frg && RANGE > 0) {
      double RANGE_over = 1./RANGE;
      for (int c = 0;c < n_chroms;c++) { // Correcting for AT run bias
	string chrom   = user_chroms[c];
	string name    = chrom;
	string name_at = name; name_at += "_at";
	cout<<"Correcting AT run bias for "<<chrom<<" ..."<<endl;
	TH1 *his_p = getHistogram(getSignalName(name,bin_size,false,false));
	if (!his_p) {
	  cerr<<"Can't find RD histogram for '"<<chrom<<"'."<<endl;
	  continue;
	}
  
	io.file.cd();
	TTree *tree = (TTree*)io.file.Get(name_at.c_str());
	if (!tree) {
	  cerr<<"Can't find AT run tree for '"<<chrom<<"'."<<endl;
	  continue;
	}
	int start,end;
	tree->SetBranchAddress("start",&start);
	tree->SetBranchAddress("end",  &end);
	int n_ent = tree->GetEntries(),atn = 2*n_ent,ati = 0;
	int *at_run = new int[atn];
	for (int i = 0;i < n_ent;i++) {
	  tree->GetEntry(i);
	  at_run[ati++] = start;
	  at_run[ati++] = end;
	}
	int nbins = his_p->GetNbinsX();

	ati = 0;
	for (int b = 1;b <= nbins;b++) {
	  double val = 0,np = 0,add = 0;
	  start = int(his_p->GetBinLowEdge(b) + 0.5);
	  end   = int(his_p->GetBinLowEdge(b) + his_p->GetBinWidth(b) + 0.5);
	  for (int p = start;p <= end;p++) {
	    double p5 = 1,p3 = 1,add5 = 0,add3 = 0;
	    while (p > at_run[ati + 1] + RANGE && ati < atn) ati += 2;
	    for (int j = ati;j < atn;j += 2) {
	      if (p < at_run[j] - RANGE) break;
	      bool is5 = at_run[j + 1] < p,is3 = at_run[j] > p;
	      int offset = 0;
	      if      (is5) offset = p - at_run[j + 1];
	      else if (is3) offset = at_run[j] - p;
	      int len     = at_run[j + 1] - at_run[j] + 1;
	      //double lost = len*0.039 - 0.59; if (lost < 0) lost = 0;
	      double lost = len*P1 + P0; if (lost < 0) lost = 0;
	      double p = (1 - lost) + lost*RANGE_over*offset;
	      if (p < 0) p = 0;
	      
	      int bin = his_read->GetBin(offset);
	      double tmp = 0;
	      if (bin >= 1 && bin <= his_read->GetNbinsX())
		tmp += NORM_READ*lost/len*his_read->GetBinContent(bin);
	      offset = int(offset - SHIFT + 0.5);
	      bin = his_frg->GetBin(offset);
	      if (bin >= 1 && bin <= his_frg->GetNbinsX())
		tmp += NORM_FRG*lost/len*his_frg->GetBinContent(offset);
	      
	      if      (is5) add5  = (add5 + tmp)*p;
	      else if (is3) add3 += tmp*p3;
	      if      (is5) p5 *= p;
	      else if (is3) p3 *= p;
	    }
	    
	    val += 0.5*p5 + 0.5*p3;
	    add += add3 + add5;
	    np++;
	  }
	  val += 0.5*add;
	  val /= np;
	  if (val > 0)
	    his_p->SetBinContent(b,his_p->GetBinContent(b)/val);
	}
	his_p->SetName(getSignalName(name,bin_size,true,false));
	io.writeHistogramsToBinDir(bin_size,his_p);
	delete[] at_run;
      }
    }
  }

  int nb_rd = 251, nb_gc = 101;
  double max_rd = estimateDepthMaximum(user_chroms,n_chroms), max_gc = 100.5;
  max_rd = (int(max_rd/(nb_rd - 1)) + 1)*(nb_rd - 1) + 0.5;

  // Statistics for uncorrected
  TH1* rd_p    = new TH1D(getDistrName(Genome::CHRALL,bin_size,false,false),
  			  "RD distribution (1-22)",nb_rd,-0.5,max_rd);
  TH1* rd_p_xy = new TH1D(getDistrName(Genome::CHRSEX,bin_size,false,false),
  			  "RD distribution (X&Y)", nb_rd,-0.5,max_rd);
  TH1 *rd_u    = new TH1D(getUDistrName(Genome::CHRALL,bin_size),
			  "RD unique distribution (1-22)",nb_rd,-0.5,max_rd);
  TH1 *rd_u_xy = new TH1D(getUDistrName(Genome::CHRSEX,bin_size),
			  "RD unique distribution (X&Y)", nb_rd,-0.5,max_rd);
  TH2 *rd_gc    = new TH2D(rd_gc_name,   "GC vs RD correlation",
			 101,-0.5,100.5,nb_rd,-0.5,max_rd);
  TH2 *rd_gc_xy = new TH2D(rd_gc_xy_name,"GC vs RD correlation for XY",
			 101,-0.5,100.5,nb_rd,-0.5,max_rd);

  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    cout<<"Making statistics for "<<chrom<<" ..."<<endl;

    TH1 *his_p  = getHistogram(getSignalName(name,bin_size,false,false));
    TH1 *his_u  = getHistogram(getUSignalName(name,bin_size));
    TH1 *his_gc = getHistogram(getGCName(name,bin_size));

    if (!his_p)
      cerr<<"Can't find RD histogram for '"<<chrom<<"'."<<endl;
    if (!his_u)
      cerr<<"Can't find unique RD histogram for '"<<chrom<<"'."<<endl;

    if (his_p) {
      int n = his_p->GetNbinsX();
      int position = 1;
      for (int i = 1;i <= n;i++) { // All RD
	double val = his_p->GetBinContent(i);
	if (Genome::isSexChrom(name)) rd_p_xy->Fill(val);
	else                          rd_p->Fill(val);
	position += bin_size;
      }
    }

    if (his_gc && his_p) { // Correlation of RD and GC
      int n = his_gc->GetNbinsX();
      if (Genome::isSexChrom(name))
	for (int i = 1;i <= n;i++) 
	  rd_gc_xy->Fill(his_gc->GetBinContent(i),
			 his_p->GetBinContent(i));
      else for (int i = 1;i <= n;i++)
	     rd_gc->Fill(his_gc->GetBinContent(i),
			 his_p->GetBinContent(i));
    }

    if (his_u) { // Unique RD
      int n = his_u->GetNbinsX();
      if (Genome::isSexChrom(name))
	for (int i = 1;i <= n;i++) rd_u_xy->Fill(his_u->GetBinContent(i));
      else
	for (int i = 1;i <= n;i++) rd_u->Fill(his_u->GetBinContent(i));
    }
  }

  double mean,sigma;
  getMeanSigma(rd_p,mean,sigma);
  cout<<"Average RD per bin (1-22) is "<<mean<<" +- "<<sigma
      <<" (before GC correction)"<<endl;
  getMeanSigma(rd_p_xy,mean,sigma);
  cout<<"Average RD per bin (X,Y)  is "<<mean<<" +- "<<sigma
      <<" (before GC correction)"<<endl;

  io.writeHistogramsToBinDir(bin_size,rd_u,rd_u_xy,rd_p,rd_p_xy,rd_gc,rd_gc_xy);

  // Correcting by GC-content
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    cout<<"Correcting counts by GC-content for '"<<chrom<<"' ..."<<endl;

    TString new_his_name = getSignalName(name,bin_size,useATcorr,true);
    TH1 *his_p  = getHistogram(getSignalName(name,bin_size,useATcorr,false));
    TH1 *his_gc = getHistogram(getGCName(name,bin_size));
    if (!his_p) continue;

    TH1 *his_corrected = his_p;
    if (!his_gc) {
      cerr<<"No histogram with GC content for '"<<chrom<<"' found."<<endl;
      cerr<<"No correction made."<<endl;
    } else {
      if (Genome::isSexChrom(name))
	//correctGCbyFragment(his_p,his_gc,rd_gc_xy,rd_p_xy);
	correctGC(his_p,his_gc,rd_gc_xy,rd_p_xy);
      else
	//correctGCbyFragment(his_p,his_gc,rd_gc,rd_p);
	correctGC(his_p,his_gc,rd_gc,rd_p);
    }
    his_corrected->SetName(new_his_name);
    io.writeHistogramsToBinDir(bin_size,his_corrected);
  }

  // Statistics for corrected counts
  TH1* rd_p_GC    = new TH1D(getDistrName(Genome::CHRALL,bin_size,useATcorr,true),
  			     "RD all (GC corrected)",   nb_rd,-0.5,max_rd);
  TH1* rd_p_xy_GC = new TH1D(getDistrName(Genome::CHRSEX,bin_size,useATcorr,true),
  			     "RD all XY (GC corrected)",nb_rd,-0.5,max_rd);
  TH2* rd_gc_GC    = new TH2D(rd_gc_GC_name,
			      "GC vs RD correlation (corrected)",
			      101,-0.5,100.5,nb_rd,-0.5,max_rd);
  TH2* rd_gc_xy_GC = new TH2D(rd_gc_xy_GC_name,
			      "GC vs RD correlation for XY (corrected)",
			      101,-0.5,100.5,nb_rd,-0.5,max_rd);


  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    cout<<"Making statistics for "<<chrom<<" after GC correction ..."<<endl;
    TH1 *his_p  = getHistogram(getSignalName(name,bin_size,useATcorr,true));
    TH1 *his_gc = getHistogram(getGCName(name,bin_size));

    if (!his_p) {
      cerr<<"Can't find histogram for '"<<chrom<<"'."<<endl;
      continue;
    }

    int n = his_p->GetNbinsX();
    if (Genome::isSexChrom(name))
      for (int i = 1;i <= n;i++) rd_p_xy_GC->Fill(his_p->GetBinContent(i));
    else
      for (int i = 1;i <= n;i++) rd_p_GC->Fill(his_p->GetBinContent(i));

    if (!his_gc) {
      cerr<<"Can't find GC-content histogram for '"<<chrom<<"'."<<endl;
      continue;
    }

    if (Genome::isSexChrom(name))
      for (int i = 1;i <= n;i++)
	rd_gc_xy_GC->Fill(his_gc->GetBinContent(i),his_p->GetBinContent(i));
    else 
      for (int i = 1;i <= n;i++)
	rd_gc_GC->Fill(his_gc->GetBinContent(i),his_p->GetBinContent(i));
  }

  getMeanSigma(rd_p_GC,mean,sigma);
  cout<<"Average RD per bin (1-22) is "<<mean<<" +- "<<sigma
      <<" (after GC correction)"<<endl;

  getMeanSigma(rd_p_xy_GC,mean,sigma);
  cout<<"Average RD per bin (X,Y)  is "<<mean<<" +- "<<sigma
      <<" (after GC correction)"<<endl;

  io.writeHistogramsToBinDir(bin_size,rd_p_GC,rd_p_xy_GC,rd_gc_GC,rd_gc_xy_GC);
}

void HisMaker::eval(string *files,int n_files,bool useATcorr,bool useGCcorr)
{
   for (int f = 0;f < n_files;f++) {
    root_file_name = files[f];
    TH1 *his    = getHistogram(getDistrName(Genome::CHRALL,bin_size,
					    useATcorr,useGCcorr));
    TH1 *his_xy = getHistogram(getDistrName(Genome::CHRSEX,bin_size,
					    useATcorr,useGCcorr));
    double mean = 0,sigma = 0;

    if (his || his_xy) cout<<"File "<<root_file_name<<endl;
    if (his) {
      getMeanSigma(his,mean,sigma);
      cout<<"Average RD per bin (1-22) is "<<mean<<" +- "<<sigma<<" "
	  <<mean/sigma<<endl;
    }

    if (his_xy) {
      getMeanSigma(his_xy,mean,sigma);
      cout<<"Average RD per bin (X,Y)  is "<<mean<<" +- "<<sigma<<" "
	  <<mean/sigma<<endl;
    }
  }
}

double HisMaker::estimateDepthMaximum(string *user_chroms,int n_chroms)
{
  double average = 0;
  int    count  = 0;
  for (int c = 0;c < n_chroms;c++) {
    string name  = user_chroms[c];
    TH1 *his_p  = getHistogram(getSignalName(name,bin_size,false,false));
    if (!his_p) continue;
    for (int b = 1;b <= his_p->GetNbinsX();b++)
      average += his_p->GetBinContent(b), count++;
  }
  double ret = 0;
  if (count <= 0) return ret;
  average /= count;
  if (2*average > ret) ret = int(2*average + 0.5) + 0.5;
  average += 10*TMath::Sqrt(average);
  if (average   > ret) ret = int(average + 0.5) + 0.5;
  return ret;
}

int HisMaker::countGCpercentage(const char *seq,int low,int up)
{
  if (low > up) return -100;
  int n_a = 0, n_t = 0, n_g = 0, n_c = 0;
  for (int p = low;p < up;p++) {
    char c = seq[p];
    if      (c == 'a' || c == 'A') n_a++;
    else if (c == 't' || c == 'T') n_t++;
    else if (c == 'g' || c == 'G') n_g++;
    else if (c == 'c' || c == 'C') n_c++;
  }
  int n_total = n_a + n_t + n_c + n_g;
  if (n_total == 0) return -100;
  return int((n_c + n_g)*100./n_total + 0.5);
}

int parseChromosomeLength(TString description)
{
  TStringToken tok(description,";");
  if (!tok.NextToken()) return -1;
  if (!tok.NextToken()) return -1;
  TString tmp = tok;
  return tmp.Atoi();
}

int getIndexForName(string name,string *arr,int n)
{
  for (int i = 0;i < n;i++)
    if (name == arr[i]) return i;
  return -1;
}

int countReads(short *arr,int s,int e)
{
  int ret = 0;
  for (int i = s;i <= e;i++) ret += arr[i];
  return ret;
}

void HisMaker::produceHistogramsNew(string *user_chroms,int user_n_chroms)
{
  if (user_chroms == NULL && user_n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting making histogram."<<endl;
    return;
  }

  string chr_names[N_CHROM_MAX] = {""};
  int    chrom_lens[N_CHROM_MAX];
  int ncs = 0;
  io.file.cd();
  TIterator *it = io.file.GetListOfKeys()->MakeIterator();
  while (TKey *key = (TKey*)it->Next()) {
    TObject *obj = key->ReadObj();
    if (obj->IsA() != TTree::Class()) {
      delete obj;
      continue;
    }
    string name  = obj->GetName();
    string title = obj->GetTitle();
    delete obj;
    if (name == "") {
      cerr<<"Tree with no name is ignored."<<endl;
      continue;
    }
    int len = parseChromosomeLength(title);
    if (len <= 0) {
      cerr<<"Can't determine chromosome length for tree '"<<name<<"'."<<endl;
      if (!refGenome_) {
	cerr<<"No reference genome specified. Aborting."<<endl;
	continue;
      }
      int c = refGenome_->getChromosomeIndex(name);
      if (c < 0) {
	cerr<<"Unknown chromosome '"<<name<<"' for genome "
	    <<refGenome_->name()<<"."<<endl;
	continue;
      }
      len = refGenome_->chromLen(c);
    }
    if (ncs < N_CHROM_MAX) {
      chr_names[ncs]  = name;
      chrom_lens[ncs] = len + 1;
      ncs++;
    } else cerr<<"Too many trees in the file '"<<root_file_name<<"'."<<endl
	       <<"Tree '"<<name<<"' is ignored."<<endl;
  }

  if (user_n_chroms == 0 || (user_n_chroms == 1 && user_chroms[0] == "")) {
    user_chroms   = chr_names;
    user_n_chroms = ncs;
  }

  int SELECT = 24;
  cout<<"Allocating memory ..."<<endl;
  short *for_pos[N_CHROM_MAX];
  Interval *ints[N_CHROM_MAX];
  for (int i = 0;i < N_CHROM_MAX;i++) {
    for_pos[i] = NULL;
    ints[i] = NULL;
  }
  for (int c = 0;c < ncs;c++) {
    //if (c != SELECT) continue;
    for_pos[c] = new short[chrom_lens[c]];
    memset(for_pos[c],0,chrom_lens[c]*sizeof(short));
  }
  cout<<"Done."<<endl;

  cout<<"Reading trees ..."<<endl;
  for (int c = 0;c < ncs;c++) {
    //if (c != SELECT) continue;
    readTreeForChromosome(root_file_name,chr_names[c],NULL,for_pos[c]);
    ints[c] = new Interval(chr_names[c] + ".alfrag");
    //cout<<chr_names[c]<<" "<<ints[c]->count()<<endl;
  }


  cout<<"Calculating values for slices ..."<<endl;
  for (int c = 0;c < user_n_chroms;c++) {
    int index = getIndexForName(user_chroms[c],chr_names,ncs);
    if (index < 0) {
      cerr<<"No data for '"<<user_chroms[c]<<"' ..."<<endl;
      continue;
    }
    for (Interval *i = ints[index];i;i = i->next()) {
      i->setUnique1(countReads(for_pos[index],i->start1(),i->end1()));
      int index = getIndexForName(i->name2(),chr_names,ncs);
      if (index < 0) continue;
      i->setOtherIndex(index);
      i->setUnique2(countReads(for_pos[index],i->start2(),i->end2()));
    }
  }

  cout<<"Assigning frequencies ..."<<endl;
  for (int c = 0;c < ncs;c++) {
    memset(for_pos[c],0,chrom_lens[c]*sizeof(short));
    for (Interval *i = ints[c];i;i = i->next()) {
      if (i->end1() >= chrom_lens[c]) continue;
      short *e = &for_pos[c][i->end1()];
      for (short *p = &for_pos[c][i->start1()];p <= e;p++) *p += 1;
    }
  }

  for (int c = 0;c < user_n_chroms;c++) {
    string name = user_chroms[c];
    int index = getIndexForName(name,chr_names,ncs);
    if (index < 0) {
      cerr<<"No data for '"<<user_chroms[c]<<"' ..."<<endl;
      continue;
    }
    cout<<"Calculating histogram with bin size of "<<bin_size<<" for "
	<<"chromosome '"<<name<<"' ..."<<endl;
    int len = chrom_lens[index];
    short  *uarr = new short[len];
    short  *rarr = new short[len];
    double *arr  = new double[len];
    memset(uarr,0,len*sizeof(short));
    memset(rarr,0,len*sizeof(short));
    memset(arr, 0,len*sizeof(double));
    readTreeForChromosome(root_file_name,name,rarr,uarr);
    Interval *fi = ints[index], *li = fi;
    for (int p = 1;p < len;p++) {
      int rand = rarr[p] - uarr[p];
      double val = uarr[p];
      if (rand != 0) {
	cout<<fi->name1()<<" "<<fi->start1()<<" "<<fi->end1()<<endl;
	while (fi && fi->end1() < p) fi = fi->next();
	int    n0  = 1;
	double det = 0;
	for (Interval *intv = fi;intv;intv = intv->next()) {
	  if (intv->start1() > p) break;
	  if (intv->end1()   < p) continue;
	  if (intv->unique1() == 0) continue;
	  int ni = for_pos[intv->otherIndex()][intv->getEquivalent(p)];
	  if (ni == 0) {
	    cerr<<"Zero ni value: "<<name<<" "<<intv->start1()<<" "
		<<intv->end1()<<" p="<<p
		<<" "<<intv->otherIndex()<<" "<<intv->getEquivalent(p)
		<<endl;
	    continue;
	  }
	  det += 1.*intv->unique2()/intv->unique1()/ni;
	  n0++;
	}
	if (n0 > 0) {
	  cout<<"p,no,det = "<<p<<" "<<n0<<" "<<1./n0 + det<<endl;
	  val += rand/(1./n0 + det);
	}
      }
      arr[p] = val;
    }

    // Making actuall histogram
    int n_bins = len/bin_size + 1,bin = 1;
    string h_name  = name; h_name += "_RD_signal";
    string h_title = name; h_name += " RD signal";
    TH1 *his_signal = new TH1D(h_name.c_str(),h_title.c_str(),n_bins,0,
			       n_bins*bin_size);
    double val = 0;
    for (int i = 1;i < len;i++) {
      if (i%bin_size == 0) {
	his_signal->SetBinContent(bin,val);
	val = 0;
	bin++;
      }
      val += arr[i];
    }
    if (bin <= bin_size) his_signal->SetBinContent(bin,val);
    io.writeHistogramsToBinDir(bin_size,his_signal);

    delete[] uarr;
    delete[] rarr;
    delete[] arr;
  }
  
  cout<<"Deleting2 ..."<<endl;
  for (int c = 0;c < ncs;c++) {
    delete[] for_pos[c];
    Interval *i = ints[c];
    while (i) {
      Interval *next = i->next();
      delete i;
      i = next;
    }
  }
  cout<<"Exiting."<<endl;
}

void HisMaker::produceHistograms(string *user_chroms,int n_chroms,
				 string *root_files,int n_root_files,
				 bool useGCcorr)
{
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting making histogram."<<endl;
    return;
  }

  if (n_root_files < 1) return;

  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithTree(chrom_names,root_files[0]);
    user_chroms = chrom_names;
  }

  cout<<"Allocating memory ..."<<endl;
  int max = 0;
  for (int c = 0;c < n_chroms;c++) {
    int len = getChromLenWithTree(user_chroms[c],root_files[0]);
    if (len > max) max = len;
    chrom_lens[c] = len;
  }
  char *seq_buffer = new char[max + 1000];
  cout<<"Done."<<endl;

  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    cout<<"Calculating histograms with bin size of "<<bin_size<<" for '"
	  <<chrom<<"' ..."<<endl;
    string name = chrom;
    int org_len = chrom_lens[c];
    if (org_len <= 0) continue;
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    TString h_name_u = getUSignalName(name,bin_size);
    TString h_name_p = getSignalName(name,bin_size,false,useGCcorr);
    TString h_title_u = "Unique read depth for "; h_title_u += name;
    TString h_title_p = "Read depth for ";        h_title_p += name;
    TH1 *his_rd_u = new TH1D(h_name_u,h_title_u,n_bins,0,len);
    TH1 *his_rd_p = new TH1D(h_name_p,h_title_p,n_bins,0,len);
    TH1 *his_gc = (TH1*)his_rd_p->Clone(getGCName(name,bin_size));
    his_rd_u->SetDirectory(0);
    his_rd_p->SetDirectory(0);
    
    for (int f = 0;f < n_root_files;f++) {
      string rfn = root_files[f];
      TFile file(rfn.c_str(),"Read");
      if (file.IsZombie()) { 
	cerr<<"Can't open file '"<<rfn<<"'."<<endl;
	continue;
      }

      // Calculating array with average RD for GC
//       int loc_bin = 1000;
//       TString loc_dir = getDirName(loc_bin);
//       TH2 *fh_rd_gc = (TH2*)getHistogram(rfn,loc_dir,"rd_gc_1000");
//       TH1 *fh_mean  = getHistogram(getDistrName(Genome::CHRALL,loc_bin,false,false),
// 				   rfn,loc_dir);
//       TH1 *fh_gc    = getHistogram(getGCName(name,loc_bin),rfn,loc_dir);
//       int N = fh_rd_gc->GetNbinsX();
//       double width = 100./(N - 1),inv_width = 1./width;
//       double *gc_corr = new double[N];
//       calcGCcorrection(fh_rd_gc,fh_mean,gc_corr,N);

      TTree *tree = (TTree*)file.Get(chrom.c_str());
      if (!tree) tree = (TTree*)file.Get(name.c_str());
      if (!tree) {
	cerr<<"Can't find tree for chromosome '"<<chrom<<"' in file '"
	    <<rfn<<"'."<<endl;
	continue;
      }
      
      int position;
      short rd_unique,rd_parity;
      tree->SetBranchAddress("position", &position);
      tree->SetBranchAddress("rd_unique",&rd_unique);
      tree->SetBranchAddress("rd_parity",&rd_parity);
      int start = 1, end = bin_size, bin = 0;
      int n_ent = tree->GetEntries(), index = 0;
      int count_unique = 0,count_parity = 0;
      tree->GetEntry(index++);
      while (index < n_ent) {
	
	// Calculate count
	while (position <= end && index < n_ent) {
	  count_unique += rd_unique;
	  count_parity += rd_parity;
	  tree->GetEntry(index++);
	  if (count_unique < 0) count_unique = 30000;
	  if (count_parity < 0) count_parity = 30000;
	}
	
	bin++;
	if (bin < 1 || bin > n_bins) {
	  cerr<<"Bin out of bounds."<<endl;
	} else {
	  double cu = his_rd_u->GetBinContent(bin) + count_unique;
	  double cp = his_rd_u->GetBinContent(bin) + count_parity;
	  his_rd_u->SetBinContent(bin,cu);
	  his_rd_p->SetBinContent(bin,cp);
	  his_rd_u->SetBinError(bin,0);
	  his_rd_p->SetBinError(bin,0);
	}
	
	start += bin_size;
	end   += bin_size;
	count_unique   = 0;
	count_parity   = 0;
      }
      delete tree;
      file.Close();
    }

    // Writing chromosome specific histograms
    io.writeHistogramsToBinDir(bin_size,his_rd_u,his_rd_p);
    
    // Creating histograms with GC-content
    cout<<"Making GC histogram for '"<<chrom<<"' ..."<<endl;
    int n_read = readChromosome(name,seq_buffer,org_len);
    if (n_read != org_len)
      cerr<<"Sequence length ("<<n_read<<") is different from expectation ("
	  <<org_len<<") for '"<<chrom<<"'."<<endl
	  <<"Doing nothing!"<<endl;
    else {
      int n = his_gc->GetNbinsX();
      for (int i = 1;i <= n;i++) {
	int low = (int) his_gc->GetBinLowEdge(i);
	int up  = (int) (low + his_gc->GetBinWidth(i));
	if (up > org_len) up = org_len;
	his_gc->SetBinContent(i,countGCpercentage(seq_buffer,low,up));
      }
      io.writeHistogramsToBinDir(bin_size,his_gc);
    }
    cout<<"Done."<<endl;

    // Deleting objects
    delete his_rd_u;
    delete his_rd_p;
    delete his_gc;
  }

  delete[] seq_buffer;
}

void HisMaker::produceHistogramsFa(string *user_chroms,int n_chroms,
         string *root_files,int n_root_files,
         bool useGCcorr)
{
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
  <<"Aborting making histogram."<<endl;
    return;
  }

  if (n_root_files < 1) return;

  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithTree(chrom_names,root_files[0]);
    user_chroms = chrom_names;
  }

  cout<<"Allocating memory ..."<<endl;
  int max = 0;
  for (int c = 0;c < n_chroms;c++) {
    int len = getChromLenWithTree(user_chroms[c],root_files[0]);
    if (len > max) max = len;
    chrom_lens[c] = len;
  }
  cout<<"Done."<<endl;

  FastaParser faparser(fastafile);
  while (faparser.nextChromosome()) {
    int c=0;
    for(;(user_chroms[c]!=faparser.getName())&&(Genome::extendedChromName(user_chroms[c])!=faparser.getName())&&(user_chroms[c]!=Genome::extendedChromName(faparser.getName()))&&(c<n_chroms);c++);
    if(c>=n_chroms) continue;
    string chrom = user_chroms[c];
    cout<<"Calculating histograms with bin size of "<<bin_size<<" for '"
    <<chrom<<"' ..."<<endl;
    string name = chrom;
    int org_len = chrom_lens[c];
    if (org_len <= 0) continue;
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    TString h_name_u = getUSignalName(name,bin_size);
    TString h_name_p = getSignalName(name,bin_size,false,useGCcorr);
    TString h_title_u = "Unique read depth for "; h_title_u += name;
    TString h_title_p = "Read depth for ";        h_title_p += name;
    TH1 *his_rd_u = new TH1D(h_name_u,h_title_u,n_bins,0,len);
    TH1 *his_rd_p = new TH1D(h_name_p,h_title_p,n_bins,0,len);
    TH1 *his_gc = (TH1*)his_rd_p->Clone(getGCName(name,bin_size));
    his_rd_u->SetDirectory(0);
    his_rd_p->SetDirectory(0);
    
    for (int f = 0;f < n_root_files;f++) {
      string rfn = root_files[f];
      TFile file(rfn.c_str(),"Read");
      if (file.IsZombie()) {
  cerr<<"Can't open file '"<<rfn<<"'."<<endl;
  continue;
      }

      // Calculating array with average RD for GC
//       int loc_bin = 1000;
//       TString loc_dir = getDirName(loc_bin);
//       TH2 *fh_rd_gc = (TH2*)getHistogram(rfn,loc_dir,"rd_gc_1000");
//       TH1 *fh_mean  = getHistogram(getDistrName(Genome::CHRALL,loc_bin,false,false),
//            rfn,loc_dir);
//       TH1 *fh_gc    = getHistogram(getGCName(name,loc_bin),rfn,loc_dir);
//       int N = fh_rd_gc->GetNbinsX();
//       double width = 100./(N - 1),inv_width = 1./width;
//       double *gc_corr = new double[N];
//       calcGCcorrection(fh_rd_gc,fh_mean,gc_corr,N);

      TTree *tree = (TTree*)file.Get(chrom.c_str());
      if (!tree) tree = (TTree*)file.Get(name.c_str());
      if (!tree) {
  cerr<<"Can't find tree for chromosome '"<<chrom<<"' in file '"
      <<rfn<<"'."<<endl;
  continue;
      }
      
      int position;
      short rd_unique,rd_parity;
      tree->SetBranchAddress("position", &position);
      tree->SetBranchAddress("rd_unique",&rd_unique);
      tree->SetBranchAddress("rd_parity",&rd_parity);
      int start = 1, end = bin_size, bin = 0;
      int n_ent = tree->GetEntries(), index = 0;
      int count_unique = 0,count_parity = 0;
      tree->GetEntry(index++);
      while (index < n_ent) {
  
  // Calculate count
  while (position <= end && index < n_ent) {
    count_unique += rd_unique;
    count_parity += rd_parity;
    tree->GetEntry(index++);
    if (count_unique < 0) count_unique = 30000;
    if (count_parity < 0) count_parity = 30000;
  }
  
  bin++;
  if (bin < 1 || bin > n_bins) {
    cerr<<"Bin out of bounds."<<endl;
  } else {
    double cu = his_rd_u->GetBinContent(bin) + count_unique;
    double cp = his_rd_u->GetBinContent(bin) + count_parity;
    his_rd_u->SetBinContent(bin,cu);
    his_rd_p->SetBinContent(bin,cp);
    his_rd_u->SetBinError(bin,0);
    his_rd_p->SetBinError(bin,0);
  }
  
  start += bin_size;
  end   += bin_size;
  count_unique   = 0;
  count_parity   = 0;
      }
      delete tree;
      file.Close();
    }

    // Writing chromosome specific histograms
    io.writeHistogramsToBinDir(bin_size,his_rd_u,his_rd_p);
    
    // Creating histograms with GC-content
    cout<<"Making GC histogram for '"<<chrom<<"' ..."<<endl;
    string cseq=faparser.getData();
    int n_read=cseq.length();
    if (n_read != org_len)
      cerr<<"Sequence length ("<<n_read<<") is different from expectation ("
    <<org_len<<") for '"<<chrom<<"'."<<endl
    <<"Doing nothing!"<<endl;
    else {
      int n = his_gc->GetNbinsX();
      for (int i = 1;i <= n;i++) {
  int low = (int) his_gc->GetBinLowEdge(i);
  int up  = (int) (low + his_gc->GetBinWidth(i));
  if (up > org_len) up = org_len;
  his_gc->SetBinContent(i,countGCpercentage(cseq.c_str(),low,up));
      }
      io.writeHistogramsToBinDir(bin_size,his_gc);
    }
    cout<<"Done."<<endl;

    // Deleting objects
    delete his_rd_u;
    delete his_rd_p;
    delete his_gc;
  }
}

double getMedian(TH1 *tmp)
{
  int nbins = tmp->GetNbinsX();
  double sum = 0, sum2 = 0;
  for (int i = 1;i <= nbins;i++) sum += tmp->GetBinContent(i);
  sum /= 2;
  for (int i = 1;i <= nbins;i++) {
    double prev = sum2;
    sum2 += tmp->GetBinContent(i);
    if (sum2 >= sum)
      return tmp->GetBinLowEdge(i) + (sum - sum2)*tmp->GetBinWidth(i);
  }
  return 0;
}

void HisMaker::produceHistograms_try_correct(string *user_chroms,int n_chroms)
{
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting making histogram."<<endl;
    return;
  }

  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithTree(chrom_names);
    user_chroms = chrom_names;
  }

  cout<<"Allocating memory ..."<<endl;
  int max = 0;
  for (int c = 0;c < n_chroms;c++) {
    int len = getChromLenWithTree(user_chroms[c]);
    if (len > max) max = len;
    chrom_lens[c] = len;
  }
  short *counts_p = new short[max + 1];
  short *counts_u = new short[max + 1];
  char  *seq_buf  = new char[max + 1000];
  cout<<"Done."<<endl;

  static const int MAX_N = 100,WIN = 2000;
  TString h_name_at  = getATaggrName();
  TString h_title_at = "Aggregation over AT runs";
  TH2 *his_at        = new TH2D(h_name_at,h_title_at,51,9.5,60.5,
				2*WIN + 1,-WIN - 0.5,WIN + 0.5);
  double half_bin    = 50./MAX_N;
  TH2 *his_test      = new TH2D("new_rd_gc","GC vs RD correlation",
				MAX_N + 1,-half_bin,MAX_N + half_bin,
				50001,-0.5,50000.5);
  TH1* rd_p          = new TH1D(getDistrName(Genome::CHRALL,bin_size,
					     false,false),
				"RD all",   50001,-0.5,50000.5);
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    int org_len = chrom_lens[c];
    if (org_len <= 0) continue;
    cout<<"Calculating histograms with bin size of "<<bin_size<<" for '"
	<<chrom<<"' ..."<<endl;
    memset(counts_p,0,(org_len + 1)*sizeof(short));
    memset(counts_u,0,(org_len + 1)*sizeof(short));
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    TString h_name_u   = getUSignalName(name,bin_size);
    TString h_name_p   = getRawSignalName(name,bin_size);
    TString h_title_u  = "Unique read depth for "; h_title_u += name;
    TString h_title_p  = "Read depth for ";        h_title_p += name;
    TH1 *his_rd_u_raw = new TH1D(h_name_u,h_title_u,n_bins,0,len);
    TH1 *his_rd_p_raw = new TH1D(h_name_p,h_title_p,n_bins,0,len);
    TH1 *his_rd_p     = (TH1*)his_rd_p_raw->Clone(getSignalName(name,bin_size,
								false,false));
    TH1 *his_gc       = (TH1*)his_rd_p_raw->Clone(getGCName(name,bin_size));
    his_rd_u_raw->SetDirectory(0);
    his_rd_p_raw->SetDirectory(0);
    his_rd_p->SetDirectory(0);
    his_gc->SetDirectory(0);

    // Reading tree into a histogram
    cout<<"Reading tree ..."<<endl;
    if (!readTreeForChromosome(root_file_name,chrom,counts_p,counts_u))
      continue;
    
    // Creating histograms with GC-content
    cout<<"Making GC histogram ..."<<endl;
    int *at_run,atn = 0;
    int n_read = readChromosome(name,seq_buf,org_len);
    if (n_read != org_len)
      atn = parseGCandAT(seq_buf,org_len,&at_run,his_gc);
    else cerr<<"Sequence length ("<<n_read
	  <<") is different from expectation ("<<org_len<<") for '"
	  <<chrom<<"'."<<endl;
    for (int p = 0;p < org_len;p++) {
      char c = seq_buf[p];
      if      (c == 'a' || c == 't' || c == 'A' || c == 'T') seq_buf[p] = '1';
      else if (c == 'g' || c == 'c' || c == 'G' || c == 'C') seq_buf[p] = '2';
    }

    // GC and RD correlation
    cout<<"Correlating RD and GC ..."<<endl;
    for (int frac = 0;frac <= MAX_N/2;frac++) {
      //cout<<"frac = "<<frac<<endl;
      char *cl = seq_buf,*cf = cl;
      int n_at = 0,n_gc = 0,ns = 0,pos = 1;
      while (pos <= org_len) {
	if (cl == cf) {
	  n_at = n_gc = 0;
	  for (int p = 1;p < MAX_N;p++) {
	    char c = *cl; cl++;
	    if (c == '1') n_at++; else if (c == '2') n_gc++;
	    if (++pos > org_len) break;
	  }
	}
	char c = *cl; cl++;
	if (c == '1') n_at++; else if (c == '2') n_gc++;
	pos++;
	if (n_at + n_gc == MAX_N && (n_at == frac || n_gc == frac)) {
	  ns++;
	  int val = 0;
	  for (int i = pos - MAX_N + 1;i <= pos;i++) val += counts_p[i];
	  his_test->Fill(n_gc,val);
	  for (char *cn = cl;cn >= cf;cn--) *cn = 'N';
	  cf = cl;
	  continue;
	}
	c = *cf; cf++;
	if (c == '1') n_at--; else if (c == '2') n_gc--;
      }
      //cout<<ns<<endl;
    }

    cout<<"Making signal histogram ..."<<endl;
    int start = 1, end = bin_size;
    for (int b = 1;b <= his_rd_p_raw->GetNbinsX();b++) {
      if (end > org_len) end = org_len;
      int val_p = 0,val_u = 0;
      for (int i = start;i <= end;i++) {
	val_p += counts_p[i];
	val_u += counts_u[i];
      }
      his_rd_p_raw->SetBinContent(b,val_p);
      his_rd_u_raw->SetBinContent(b,val_u);
      rd_p->Fill(val_p);
      start += bin_size;
      end   += bin_size;
    }

//     TFile file(root_file_name,"Read");
//     if (file.IsZombie()) { 
//       cerr<<"Can't open file '"<<root_file_name<<"'."<<endl;
//       return;
//     }
//     int position;
//     short rd_unique,rd_parity;
//     TTree *tree = (TTree*) file.Get(name.c_str());
//     if (!tree) continue;
//     tree->SetBranchAddress("position", &position);
//     tree->SetBranchAddress("rd_unique",&rd_unique);
//     tree->SetBranchAddress("rd_parity",&rd_parity);
//     int n_ent = tree->GetEntries(),ati = 0;
//     for (int i = 0;i < n_ent;i++) { // Aggregating
//       tree->GetEntry(i);
//       while (ati < atn && position > at_run[ati + 1] + WIN) ati += 2;
//       for (int j = ati;j < atn;j += 2) {
// 	if (position < at_run[j] - WIN) break;
// 	int len = at_run[j + 1] - at_run[j] + 1;
// 	int offset = position - at_run[j];
// 	if (offset > 0) {
// 	  offset = position - at_run[j + 1];
// 	  if (offset < 0) offset = 0;
// 	}
// 	for (int i = 0;i < rd_parity;i++) his_at->Fill(len,offset);
//       }
//     }
    
//     // Deleting tree
//     delete tree;
//     file.Close();

//     // Deleting objects
//     cout<<"Filling and saving tree with AT runs ..."<<endl;
//     writeATTreeForChromosome(name,at_run,atn);
//     if (atn > 0) delete[] at_run;

    // Writing chromosome specific histograms
    io.writeHistogramsToBinDir(bin_size,his_rd_u_raw,his_rd_p_raw,his_rd_p,his_gc);
    delete his_rd_u_raw;
    delete his_rd_p_raw;
    delete his_rd_p;
    delete his_gc;
  }

  double av_for_gc[MAX_N + 1];
  for (int i = 0;i <= MAX_N;i++) {
    TH1 *tmp = his_test->ProjectionY("tmp",i + 1,i + 1);
    //av_for_gc[i] = tmp->GetMean();
    av_for_gc[i] = getMedian(tmp);
    //cout<<i<<" "<<tmp->GetMean()<<" "<<av_for_gc[i]<<endl;
  }
  TH2 *his_read_frg = (TH2*)getHistogram(root_file_name,"","read_frg_len");
  TH1 *his_frg  = his_read_frg->ProjectionY("his_frg"); normalize(his_frg);

  double average = getMean(rd_p),av_over = bin_size/average/MAX_N;
  cout<<"average = "<<average<<endl;
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    string name  = chrom;
    int org_len  = chrom_lens[c];
    if (org_len <= 0) continue;

    cout<<"Correcting signal for '"<<chrom<<"' ..."<<endl;
    memset(counts_p,0,(org_len + 1)*sizeof(short));
    TH1 *his_rd_p = getHistogram(root_file_name,dir_name,
				 getSignalName(name,bin_size,false,false));

    cout<<"Reading tree ..."<<endl;
    if (!readTreeForChromosome(root_file_name,chrom,counts_p,NULL))
      continue;

    cout<<"Reading sequence ..."<<endl;
    int n_read = readChromosome(name,seq_buf,org_len);
    if (n_read != org_len)
      cerr<<"Sequence length ("<<n_read
	  <<") is different from expectation ("<<org_len<<") for '"
	  <<chrom<<"'."<<endl;
    for (int p = 0;p < org_len;p++) {
      char c = seq_buf[p];
      if      (c == 'a' || c == 't' || c == 'A' || c == 'T') seq_buf[p] = '1';
      else if (c == 'g' || c == 'c' || c == 'G' || c == 'C') seq_buf[p] = '2';
    }

    cout<<"Correcting ..."<<endl;
    int bstart = 1, bend = bin_size;
    for (int b = 1;b <= his_rd_p->GetNbinsX();b++) {
      if (bstart > 2000000) break;
      if (bend > org_len) bend = org_len;
      double val = 0;
      for (int i = bstart;i <= bend;i++) {
	if (counts_p[i] <= 0) continue;
	if (i < 1000) continue;
	// Calculating correction
	double corr1 = 0,corr2 = 0;
 	int n_at = 0,n_gc = 0,max_at = 0,max_gc = 0,p = 0;
 	char *cf  = &seq_buf[i - 1],   *cl = cf;
	char *end = &seq_buf[org_len], *cend = cl + MAX_N - 1;
	if (cend > end) cend = end;
	while (cl < cend) {
	  char c = *cl; cl++; p++;
	  if (c == '1') n_at++; else if (c == '2') n_gc++;
	}
 	cend = cl + 1000; if (cend > end) cend = end;
	while (cl < cend) {
	  char c = *cl; cl++;
	  if (c == '1') n_at++; else if (c == '2') n_gc++;
	  if (n_at > max_at) max_at = n_at;
	  if (n_gc > max_gc) max_gc = n_gc;
	  c = *cf; cf++;
	  if (c == '1') n_at--; else if (c == '2') n_gc--;
	  double reduce = av_for_gc[MAX_N - max_at];
	  if (av_for_gc[max_gc] < reduce) reduce = av_for_gc[max_gc];
	  corr1 += reduce*av_over*his_frg->GetBinContent(p++);
	}
 	n_at = n_gc = max_at = max_gc = p = 0;
 	cl = cf = &seq_buf[i - 1];
	char *start =  seq_buf, *cstart = cl - MAX_N + 1;
	if (cstart < start) cstart = start;
	while (cl > cstart) {
	  char c = *cl; cl--; p++;
	  if (c == '1') n_at++; else if (c == '2') n_gc++;
	}
 	cstart = cl - 1000; if (cstart < start) cstart = start;
	while (cl > cstart) {
	  char c = *cl; cl--;
	  if (c == '1') n_at++; else if (c == '2') n_gc++;
	  if (n_at > max_at) max_at = n_at;
	  if (n_gc > max_gc) max_gc = n_gc;
	  c = *cf; cf--;
	  if (c == '1') n_at--; else if (c == '2') n_gc--;
	  double reduce = av_for_gc[MAX_N - max_at];
	  if (av_for_gc[max_gc] < reduce) reduce = av_for_gc[max_gc];
	  corr2 += reduce*av_over*his_frg->GetBinContent(p++);
	}
	val += 2*counts_p[i]/(corr1 + corr2);
      }
      
      his_rd_p->SetBinContent(b,val);
      bstart += bin_size;
      bend   += bin_size;
    }
    io.writeHistogramsToBinDir(bin_size,his_rd_p);
  }

  // Writing global histograms
  io.writeHistogramsToBinDir(bin_size,his_at,his_test);
  delete his_at;
  delete his_test;

  // Copying histograms
  writeHistograms(his_read_frg);

  delete[] counts_p;
  delete[] counts_u;
  delete[] seq_buf;
}

TH1* makeFrgLenHis(string *files,int n_files,int max_ins_len)
{
  TH2 *read_frg_len_his = NULL;
  for (int f = 0;f < n_files;f++) {
    string fileName = files[f];
    TFile file(fileName.c_str());
    if (file.IsZombie()) {
      cerr<<"Can't open/read file '"<<fileName<<"'."<<endl;
      continue;
    }
    TH2 *tmp = (TH2*) file.Get("read_frg_len");
    if (read_frg_len_his) read_frg_len_his->Add(tmp);
    else {
      gROOT->cd();
      read_frg_len_his = (TH2*)tmp->Clone("tmp_his");
    }
    file.Close();
  }
  if (!read_frg_len_his) return NULL;

  TH1 *rl = read_frg_len_his->ProjectionX("rl");
  TH1 *fl = read_frg_len_his->ProjectionY("fl");
  TH1 *frg_len_his = new TH1D("frg_len_his","Fragment-read length",
			      max_ins_len,0.5,max_ins_len + 0.5);
  int nbinsx = read_frg_len_his->GetNbinsX();
  int nbinsy = read_frg_len_his->GetNbinsY();
  for (int x = 1;x <= nbinsx;x++) {
    double read_len = rl->GetBinCenter(x)*0.5;
    for (int y = 1;y <= nbinsy;y++) {
      int frg_len = int(fl->GetBinCenter(y) - read_len + 0.5);
      if (frg_len > 0 && frg_len <= max_ins_len) {
	double val = frg_len_his->GetBinContent(frg_len);
	val += read_frg_len_his->GetBinContent(x,y);
	frg_len_his->SetBinContent(frg_len,val);
      }
    }
  }

  double norm = 0;
  for (int b = 1;b <= max_ins_len;b++) norm += frg_len_his->GetBinContent(b);
  if (norm > 0) frg_len_his->Scale(1./norm);

  return frg_len_his;
}

void HisMaker::aggregate(string *files,int n_files,string *chroms,int n_chroms)
{
  static const int WIN = 2000;
  TH2 *his_AT_aggr = new TH2D("his_AT_aggr","AT aggregation",51,9.5,60.5,
			      2*WIN + 1,-WIN - 0.5,WIN + 0.5);
  TH2 *his_AT_corr = new TH2D("his_AT_aggr_corr","Corrected AT aggregation",
			      51,9.5,60.5,
			      2*WIN + 1,-WIN - 0.5,WIN + 0.5);
  TH2 *his_GC_aggr = new TH2D("his_GC_aggr","GC aggregation",51,9.5,60.5,
			      2*WIN + 1,-WIN - 0.5,WIN + 0.5);

  if (chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
  	<<"Aborting aggregation."<<endl;
    return;
  }

  static const int RANGE = 550;
  TH1 *his_frg_len = makeFrgLenHis(files,n_files,2*RANGE);

  char  *seq_buffer = new char[250000000];
  int   *at_starts  = new int[900000];
  int   *at_ends    = new int[900000];
  int   *gc_starts  = new int[400000];
  int   *gc_ends    = new int[400000];
  for (int chr = 0;chr < n_chroms;chr++) {
    string name = chroms[chr];
    cout<<"Aggregating for "<<name<<" ..."<<endl;
    int atn = 0,gcn = 0;
    int len_seq = readChromosome(name,seq_buffer,250000000);
    for (int i = 0;i < len_seq;i++) {
      char c;
      int ats = i,ate = i;
      while (i < len_seq && (c = seq_buffer[i]) &&
	     (c == 'A' || c == 'a' || c == 'T' || c == 't')) ate = i++;
      
      int at_len = ate - ats + 1;
      if (at_len >= 10) {
	at_starts[atn] = ats + 1;
	at_ends[atn]   = ate + 1;
	atn++;
      }
      int gcs = i,gce = i;
      while (i < len_seq && (c = seq_buffer[i]) &&
	     (c == 'C' || c == 'c' || c == 'G' || c == 'g')) gce = i++;
      if (gce - gcs + 1 >= 10) {
	gc_starts[gcn] = gcs + 1;
	gc_ends[gcn]   = gce + 1;
	gcn++;
      }
      if (i > ats) i--;
    }

    cout<<atn<<" "<<gcn<<endl;

    for (int f = 0;f < n_files;f++) {
      string fileName = files[f];
      cout<<"Readng tree "<<name<<" from file '"<<fileName<<"' ..."<<endl;
      TFile file(fileName.c_str());
      if (file.IsZombie()) {
	cerr<<"Can't open/read file '"<<fileName<<"'."<<endl;
	continue;
      }
      TTree *tree = (TTree*)file.Get(name.c_str());
      if (!tree)
	tree = (TTree*)file.Get(Genome::canonicalChromName(name).c_str());
      if (!tree) {
	cerr<<"Can't find tree for '"<<name<<"' in file '"
	    <<fileName<<"'."<<endl;
	continue;
      }
      int position,ati = 0,gci = 0;
      short rd_unique,rd_parity;
      tree->SetBranchAddress("position", &position);
      tree->SetBranchAddress("rd_unique",&rd_unique);
      tree->SetBranchAddress("rd_parity",&rd_parity);
      int n_ent = tree->GetEntries();
      for (int ent = 0;ent < n_ent;ent++) {
	tree->GetEntry(ent);
	while (position > at_ends[ati] + WIN && ati < atn) ati++;
	double p5 = 1,p3 = 1,range_over = 1./RANGE,add5 = 0,add3 = 0;
	int offset = 0;
	for (int i = ati;i < atn;i++) {
	  bool is5 = at_ends[i] < position,is3 = at_starts[i] > position;
	  if        (is5) {
	    offset = position - at_ends[i];
	    if (offset > RANGE) continue;
	  } else if (is3) {
	    offset = at_starts[i] - position;
	    if (offset > RANGE) break;
	  } else continue;
	  int len     = at_ends[i] - at_starts[i] + 1;
	  double lost = len*0.039 - 0.59;
	  if (lost < 0) lost = 0; // if (lost > 1) lost = 1;
	  double p = (1 - lost) + lost*range_over*offset;
	  if (p < 0) p = 0;

	  double tmp = 0;
	  offset += 8;
	  int bin = his_frg_len->GetBin(offset);
	  if (bin >= 1 && bin <= his_frg_len->GetNbinsX())
	    tmp += 2100*lost/len*his_frg_len->GetBinContent(offset);

	  if      (is5) add5  = (add5 + tmp)*p;
	  else if (is3) add3 += tmp*p3;
	  if      (is5) p5 *= p;
	  else if (is3) p3 *= p;
	}

	double val = 0.5*p5 + 0.5*p3 + (add5 + add3)*0.5;
	if (val < 0.001) {
	  //cout<<"val = "<<val<<endl;
	  if (val < 0.001) val = 0.001;
	}

	for (int j = ati;j < atn;j++) {
	  if (position < at_starts[j] - WIN) break;
	  int len = at_ends[j] - at_starts[j] + 1;
	  if (len > 60) len = 60;
	  int offset = position - at_starts[j];
	  if (offset > 0) {
	    offset = position - at_ends[j];
	    if (offset < 0) offset = 0;
	  }
	  int x = len - 9,y = offset + WIN + 1;
	  his_AT_aggr->SetBinContent(x,y,his_AT_aggr->GetBinContent(x,y) + rd_parity);
 	  double rdp = rd_parity;
 	  if (val > 0) rdp = rd_parity/val;
	  his_AT_corr->SetBinContent(x,y,his_AT_corr->GetBinContent(x,y) + rdp);
	}
	while (position > gc_ends[gci] + WIN && gci < gcn) gci++;
	for (int j = gci;j < gcn;j++) {
	  if (position < gc_starts[j] - WIN) break;
	  int len = gc_ends[j] - gc_starts[j] + 1;
	  if (len > 60) len = 60;
	  int offset = position - gc_starts[j];
	  if (offset > 0) {
	    offset = position - gc_ends[j];
	    if (offset < 0) offset = 0;
	  }
	  for (int i = 0;i < rd_parity;i++) his_GC_aggr->Fill(len,offset);
	}
      }

      file.Close();
    }
  }

  writeHistograms(his_AT_aggr,his_AT_corr,his_GC_aggr,his_frg_len);
  
  delete[] seq_buffer;
  delete[] at_starts;
  delete[] at_ends; 
  delete[] gc_starts;
  delete[] gc_ends; 
}

void HisMaker::mergeTrees(string *user_chroms,int n_chroms,
			  string *user_files,int n_files)
{
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<<endl
	<<"Aborting merging trees."<<endl;
    return;
  }

  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms       = getChromNamesWithTree(chrom_names,user_files[0]);
    user_chroms    = chrom_names;
  }

  cout<<"Allocating memory ..."<<endl;
  int max = 0;
  for (int c = 0;c < n_chroms;c++) {
    int len = getChromLenWithTree(user_chroms[c],user_files[0]);
    if (len > max) max = len;
    chrom_lens[c] = len;
  }
  short *counts_u = new short[max + 1];
  short *counts_p = new short[max + 1];
  for (int c = 0;c < n_chroms;c++) {
    if (chrom_lens[c] <= 0) continue;
    string chrom = user_chroms[c];
    cout<<"Merging trees for '"<<chrom<<"' ..."<<endl;
    memset(counts_u,0,(max + 1)*sizeof(short));
    memset(counts_p,0,(max + 1)*sizeof(short));
    for (int f = 0;f < n_files;f++) {
      cout<<"Readng tree "<<chrom<<" from file '"<<user_files[f]
	  <<"' ..."<<endl;
      readTreeForChromosome(user_files[f],chrom,counts_p,counts_u);
    }
    cout<<"Filling and saving tree for '"<<chrom<<"' ..."<<endl;
    writeTreeForChromosome(chrom,&counts_p[1],&counts_u[1],chrom_lens[c]);
  }
  delete[] counts_u;
  delete[] counts_p;
}

int HisMaker::getChromNamesWithTree(string *names,string rfn,bool vcf)
{
  if (!names) return 0;
  if (rfn.length() == 0) rfn = root_file_name;
  TFile file(rfn.c_str(),"Read");
  if (file.IsZombie()) {
    cerr<<"Can't open file '"<<root_file_name<<"'."<<endl;
    return 0;
  }
  int ret = 0;
  TIterator *it = file.GetListOfKeys()->MakeIterator();
  while (TKey *key = (TKey*)it->Next()) {
    TObject *obj = key->ReadObj();
    if (obj->IsA() != TTree::Class()) continue;
    string chrom = obj->GetName();
    if (chrom == "") {
      cerr<<"Tree with no name is ignored."<<endl;
      continue;
    }
    string vcfstr="snp_";
    if (!vcf && (chrom.compare(0,vcfstr.size(),vcfstr)==0)) {
      continue;
    }
    if (vcf && (chrom.compare(0,vcfstr.size(),vcfstr)!=0)) {
      continue;
    }
    if (ret >= N_CHROM_MAX) {
      cerr<<"Too many trees in the file '"<<root_file_name<<"'."<<endl
      <<"Tree '"<<chrom<<"' is ignored."<<endl;
      continue;
    }
    if(vcf) names[ret++] = chrom.substr(vcfstr.size());
    else names[ret++] = chrom;
  }
  file.Close();
  return ret;
}


int HisMaker::getChromLenWithTree(string chrom,string rfn)
{
  int len = 0;
  string name  = chrom;
  if (rfn.length() == 0) rfn = root_file_name;
  TFile file(rfn.c_str(),"Read");
  if (!file.IsZombie()) { 
    TTree *tree = (TTree*) file.Get(name.c_str());
    if (tree) len = parseChromosomeLength(tree->GetTitle());
  }
  file.Close();

  if (len <= 0) {
    cerr<<"Can't determine length for '"<<chrom<<"'."<<endl;
    if (!refGenome_) cerr<<"No reference genome specified."<<endl;
    else {
      int c = refGenome_->getChromosomeIndex(name);
      if (c >= 0) len = refGenome_->chromLen(c);
      else cerr<<"Unknown chromosome/contig '"<<chrom<<"' for genome "
	       <<refGenome_->name()<<"."<<endl;
    }
  }
  
  return len;
}

int HisMaker::parseGCandAT(char *seq,int len,int **addr_for_at,TH1 *his_gc)
{
  int *at_run = new int[len];
  int atn = 0;
  for (int i = 0;i < len;i++) {
    int ats = i,ate = i;
    char c;
    while (i < len && (c = seq[i]) &&
	   (c == 'A' || c == 'a' || c == 'T' || c == 't')) ate = i++;
    int at_len = ate - ats + 1;
    if (at_len >= 10) {
      at_run[atn++] = ats + 1;
      at_run[atn++] = ate + 1;
    }
  }

  int *at_se = new int[atn];
  for (int i = 0;i < atn;i++) at_se[i] = at_run[i];
  *addr_for_at = at_se;

  if (his_gc) {
    int n = his_gc->GetNbinsX();
    for (int i = 1;i <= n;i++) {
      int low = (int) his_gc->GetBinLowEdge(i);
      int up  = (int) (low + his_gc->GetBinWidth(i));
      if (up > len) up = len;
      his_gc->SetBinContent(i,countGCpercentage(seq,low,up));
    }
  }

  delete[] at_run;

  return atn;
}

int findIndex(string *arr,int n,string name)
{
  string can_name = Genome::canonicalChromName(name);
  for (int i = 0;i < n;i++)
    if (Genome::canonicalChromName(arr[i]) == can_name) return i;
  return -1;
}

void HisMaker::produceTreesFrom1BAM(string *user_chroms,int n_chroms,
				    string user_file,bool lite)
{
  AliParser *parser = new AliParser(user_file);
  for (int i = 0;i < n_chroms;i++) {
    string chr = user_chroms[i];
    cout<<"We have chromosome "<<chr<<endl;
    if (parser->scrollTo(chr,0) < 0) {
      cout<<"Can't scroll to chromosome "<<chr<<endl;
      continue;
    }
    cout<<"About to parse "<<endl;
    while (parser->parseRecord()) {
      if (parser->isUnmapped())  continue;
      if (parser->isDuplicate()) continue;
      if (parser->isSecondary()) continue;
      cout<<"We parsed "<<parser->getChromosome()<<endl;
      break;
    }
  }
  delete parser;
}

void HisMaker::produceTrees(string *user_chroms,int n_chroms,
			    string *user_files,int n_files,
			    bool lite)
{
//   if (n_files == 1 && AliParser::looksLikeBAM(user_files[0]))
//     return produceTreesFrom1BAM(user_chroms,n_chroms,user_files[0],lite);

  string one_string[1] = {""};
  if (user_chroms == NULL) n_chroms = 0;
  if (user_files  == NULL || n_files == 0) {
    n_files    = 1;
    user_files = one_string;
  }

  string cnames[N_CHROM_MAX];
  int    clens[N_CHROM_MAX],reindex[N_CHROM_MAX],ncs = 0,atlens[N_CHROM_MAX];
  short *counts_u[N_CHROM_MAX],*counts_p[N_CHROM_MAX];
  int   *at_se[N_CHROM_MAX];
  for (int i = 0;i < N_CHROM_MAX;i++) {
    counts_u[i] = counts_p[i] = NULL;
    at_se[i] = NULL;
    atlens[i] = -1;
  }
  THashTable unknown;

  static const int WIN = 2000;
  TH2 *his_at_aggr  = new TH2I("his_at_aggr","AT aggregation",51,9.5,60.5,
			       2*WIN + 1,-WIN - 0.5,WIN + 0.5);
  TH2* his_frg_read = new TH2I("read_frg_len","Read and fragment lengths",
			       300,0.5,300.5,3001,-0.5,3000.5);
  int win = WIN/2;
  TH3* his_pair_pos = new TH3I("pair_pos","Pair position relative to AT run",
			       51,9.5,60.5,
			       2*win + 1,-win - 0.5,win + 0.5,
			       2*win + 1,-win - 0.5,win + 0.5);
  

  long n_placed = 0;
  int ati = 0;
  for (int f = 0;f < n_files;f++) {

    if (user_files[f].length() > 0)
      cout<<"Parsing file "<<user_files[f]<<" ..."<<endl;
    else 
      cout<<"Parsing stdin ..."<<endl;

    AliParser *parser = new AliParser(user_files[f].c_str());
    bool use_ref = false;
    if (parser->numChrom() == 0) {
      use_ref = true;
      cout<<"No chromosome/contig description given."<<endl;
      if (!refGenome_) {
	cerr<<"No reference genome specified. Aborting parsing."<<endl;
	continue;
      }
      cout<<"Using "<<refGenome_->name()<<" as reference genome."<<endl;
      for (int c = 0;c < refGenome_->numChrom();c++) reindex[c] = -1;
      for (int c = 0;c < refGenome_->numChrom();c++) {
	string name = refGenome_->chromName(c);
	if (n_chroms > 0 && findIndex(user_chroms,n_chroms,name) < 0) {
	  //cout<<"NOT considering chromosome/contig '"<<name<<"'."<<endl;
	  continue;
	}
	int index = findIndex(cnames,ncs,name);
	if (index < 0) {
	  index = ncs++;
	  cnames[index] = name;
	  clens[index]  = refGenome_->chromLen(c);
	}
	if (clens[index] != refGenome_->chromLen(c))
	  cerr<<"Different lengths for '"<<name<<"' "
	      <<"("<<clens[index]<<", "<<refGenome_->chromLen(c)<<")."<<endl
	      <<"Using the previous length "<<clens[index]<<endl;
	reindex[c] = index;
      }
    } else {
      for (int c = 0;c < parser->numChrom();c++) reindex[c] = -1;
      for (int c = 0;c < parser->numChrom();c++) {
	string name = parser->chromName(c);
	if (n_chroms > 0 && findIndex(user_chroms,n_chroms,name) < 0) {
	  //cout<<"NOT considering chromosome/contig '"<<name<<"'."<<endl;
	  continue;
	}
	int index = findIndex(cnames,ncs,name);
	if (index < 0) {
	  index = ncs++;
	  cnames[index] = name;
	  clens[index]  = parser->chromLen(c);
	}
	if (clens[index] != parser->chromLen(c))
	  cerr<<"Different lengths for '"<<name<<"' "
	      <<"("<<clens[index]<<", "<<parser->chromLen(c)<<")."<<endl
	      <<"Using the previous length "<<clens[index]<<endl;
	reindex[c] = index;
      } 
    }

    cout<<"Allocating memory ..."<<endl;
    for (int c = 0;c < ncs;c++) {
      if (!counts_p[c]) {
	counts_p[c] = new short[clens[c] + 1];
	memset(counts_p[c],0,(clens[c] + 1)*sizeof(short));
      }
      if (!counts_u[c]) {
	counts_u[c] = new short[clens[c] + 1];
	memset(counts_u[c],0,(clens[c] + 1)*sizeof(short));
      }
    }
    cout<<"Done."<<endl;

    int    prev_chr_ind = -1,chr_ind;
    string prev_chr("");
    while (parser->parseRecord()) {
      if (parser->isUnmapped())  continue;
      if (parser->isDuplicate()) continue;
      if (parser->isSecondary()) continue;

      if (use_ref) { // Using reference genome
	string chr = parser->getChromosome();
	if (chr == prev_chr) chr_ind = prev_chr_ind;
	else {
	  chr_ind      = refGenome_->getChromosomeIndex(chr);
	  prev_chr     = chr;
	  if (chr_ind < 0 && !unknown.FindObject(chr.c_str())) {
	      cerr<<"Unknown chromosome/contig '"<<chr<<"' for genome "
		  <<refGenome_->name()<<"."<<endl;
	      unknown.Add(new TNamed(chr.c_str(),""));
	  }
	}
      } else chr_ind = parser->getChromosomeIndex(); // bam/sam

      if (chr_ind < 0) continue;
      chr_ind = reindex[chr_ind];
      if (chr_ind < 0 || chr_ind >= ncs) continue;
      int mid = abs(parser->getStart() + parser->getEnd())>>1;
      if (mid < 0 || mid > clens[chr_ind]) {
	cerr<<"Out of bound coordinate "<<mid<<" for '"
	    <<parser->getChromosome()<<"'."<<endl;
	continue;
      }

      // Doing counting
      if (counts_p[chr_ind][mid] + 1 > 0) counts_p[chr_ind][mid]++;
      if (!parser->isQ0())
	if (counts_u[chr_ind][mid] + 1 > 0) counts_u[chr_ind][mid]++;
      n_placed++;
      
      int frg_len = parser->getFragmentLength();
      if (frg_len < 0) his_frg_read->Fill(parser->getReadLength(),-frg_len);
      else             his_frg_read->Fill(parser->getReadLength(), frg_len);

//       // Parsing AT runs
//       if (atlens[chr_ind] < 0) {
// 	int len = clens[chr_ind];
// 	char *seq_buffer = new char[len + 1000];
// 	if (readChromosome(cnames[chr_ind],seq_buffer,len) == len)
// 	  atlens[chr_ind] = parseGCandAT(seq_buffer,len,&at_se[chr_ind]);
// 	else cerr<<"Sequence length is different from expectation."<<endl;
// 	if (atlens[chr_ind] < 0) atlens[chr_ind] = 0;
// 	delete[] seq_buffer;
//       }
//       if (chr_ind != prev_chr_ind) ati = 0;

//       // Aggregating over AT runs
//       int atn = atlens[chr_ind];
//       if (atn > 0) {
// 	int mid2 = 0;
// 	if (frg_len > 0)
// 	  mid2 = parser->getStart() + frg_len - (mid - parser->getStart()) - 1;
// 	if (frg_len < 0)
// 	  mid2 = parser->getEnd()   + frg_len + (mid - parser->getStart()) + 1;
// 	int *at_run = at_se[chr_ind];
// 	ati++;
// 	while (at_run[ati] + WIN > mid && ati > 0)   ati -= 2;
// 	while (mid > at_run[ati] + WIN && ati < atn) ati += 2;
// 	ati--;
// 	for (int j = ati;j < atn;j += 2) {
// 	  if (mid < at_run[j] - WIN) break;
// 	  int len = at_run[j + 1] - at_run[j] + 1;
// 	  if (len > 60) len = 60;
// 	  int offset1 = mid - at_run[j],offset2 = mid2 - at_run[j];
// 	  if (offset1 > 0) {
// 	    offset1 = mid - at_run[j + 1];
// 	    if (offset1 < 0) offset1 = 0;
// 	  }
// 	  if (offset2 > 0) {
// 	    offset2 = mid2 - at_run[j + 1];
// 	    if (offset2 < 0) offset2 = 0;
// 	  }
// 	  his_at_aggr->Fill(len,offset1);
// 	  if (frg_len != 0)
// 	    his_pair_pos->Fill(len,offset1,offset2);
// 	}
//       }

      prev_chr_ind = chr_ind;
    }
    delete parser;
  }

  for (int c = 0;c < ncs;c++) 
    if (counts_p[c]) {
      cout<<"Filling and saving tree for '"<<cnames[c]<<"' ..."<<endl;
      short *arru = NULL, *arrp = NULL;
      if (counts_p[c]) arrp = &counts_p[c][1];
      if (counts_u[c]) arru = &counts_u[c][1];

      if (lite) // Aggregating by 100 consequites positions
	for (int pos = 1;pos < clens[c];pos += 100) {
	  int i = pos + 1;
	  while (i < clens[c] && i < pos + 100) {
	    arrp[pos] += arrp[i];
	    arru[pos] += arru[i];
	    arrp[i]    = arru[i] = 0;
	    i++;
	  }
      }

      writeTreeForChromosome(cnames[c],arrp,arru,clens[c]);
    }

  cout<<"Writing histograms ... "<<endl;
  writeHistograms(his_frg_read,his_at_aggr,his_pair_pos);

//   cout<<"Saving RD counts ..."<<endl;
//   int n_vals = 9000000;
//   char  *bin_vals = new char[n_vals];
//   ofstream bin_file("data.bin",ios::out | ios::binary);
//   for (int c = 0;c < ncs;c++) {
//     if (!counts_p[c]) continue;
//     short *arrp = &counts_p[c][1], *arru = &counts_u[c][1];
//     int n_save = 0, pos = 0;
//     for (int i = 100;i < clens[c];i += 100) {
//       short val_p = 0, val_u = 0;
//       while (pos < i) {
//         val_p += arrp[pos];
//         val_u += arru[pos];
//         pos++;
//       }
//       if (val_p > 4095) val_p = 4095; // max with 12 bits
//       if (val_u > 4095) val_u = 4095; // max with 12 bits
//       bin_vals[n_save++] = (val_p >> 4) & 0xff;
//       bin_vals[n_save++] = ((val_p << 4) & 0xf0) + ((val_u >> 8) & 0x0f);
//       bin_vals[n_save++] = val_u & 0xff;
//     }
//     bin_file.write(bin_vals,n_save);
//   }
//   bin_file.close();

  // Reading RD counts
//   char buffer[100];
//   ifstream bin_file ("data.bin", ios::in | ios::binary);

//   short val1 = 0, val2 = 0;

//   int n = 0;
//   while (bin_file.read(buffer,3)) {
//     n++;
//     val1 = buffer[0] & 0xff;
//     val1 <<= 4;
//     val1 += (buffer[1] & 0xf0) >> 4;
//     val2 =  buffer[1] & 0x0f;
//     val2 <<= 8;
//     val2 += buffer[2] & 0xff;
//     if (val1 > 10 || val2 > 10)
//       cout<<val1<<" "<<val2<<endl;
//   }
//   cout<<"n = "<<n<<endl;

  for (int c = 0;c < ncs;c++) {
    delete[] counts_u[c];
    delete[] counts_p[c];
    delete[] at_se[c];
  }

  cout<<"Total of "<<n_placed<<" reads were placed."<<endl;
}

void HisMaker::addVcf(string *user_chroms,int n_chroms,string *user_files,int n_files,bool rmchr,bool addchr)
{
  string one_string[1] = {""};
  if (user_chroms == NULL) n_chroms = 0;
  if (user_files  == NULL || n_files == 0) {
    n_files    = 1;
    user_files = one_string;
  }
  std::vector<int>  vcf_position;
  std::vector<char> vcf_ref;
  std::vector<char> vcf_alt;
  std::vector<short> vcf_qual;
  std::vector<short> vcf_nref;
  std::vector<short> vcf_nalt;
  std::vector<short> vcf_gt;
  std::vector<short> vcf_flag;
  long n_placed = 0;
  if (user_files[0].length() > 0)
    cout<<"Parsing file "<<user_files[0]<<" ..."<<endl;
  else
    cout<<"Parsing stdin ..."<<endl;
  VcfParser *parser = new VcfParser(user_files[0]);
  string prev_chr("");
  while (parser->parseRecord()) if(parser->getFilter()=="PASS") {
    string chr=parser->getChromosome();
    if(addchr) chr=Genome::extendedChromName(chr);
    if(rmchr) chr=Genome::canonicalChromName(chr);
    if (n_chroms > 0 && findIndex(user_chroms,n_chroms,chr) < 0) continue;
    if(chr!=prev_chr) {
      if(!vcf_position.empty()) writeTreeForVcf(prev_chr,vcf_position,vcf_ref,vcf_alt,vcf_qual,vcf_nref,vcf_nalt,vcf_gt,vcf_flag);
      vcf_position.clear();
      vcf_ref.clear();
      vcf_alt.clear();
      vcf_qual.clear();
      vcf_nref.clear();
      vcf_nalt.clear();
      vcf_gt.clear();
      vcf_flag.clear();
    }
    int pos=parser->getPosition();
    string tref=parser->getRef();
    string talt=parser->getAlt();
    if(tref.length()!=1 && tref.length()!=2) {
      continue;
    }
    if(talt.length()!=1 && talt.length()!=2) {
      continue;
    }
    if(tref.length()==2 && talt.length()==2) {
      continue;
    }
    vcf_position.push_back(pos);
    char ttref=tref[0];
    char ttalt=talt[0];
    if(tref.length()==2) {
      ttref=tref[1];
      ttalt='-';
    }
    if(talt.length()==2) {
      ttalt=talt[1];
      ttref='-';
    }
    vcf_ref.push_back(ttref);
    vcf_alt.push_back(ttalt);
    vcf_qual.push_back(static_cast<short>(parser->getQual()));
    vcf_nref.push_back(parser->getNRef());
    vcf_nalt.push_back(parser->getNAlt());
    string tgt=parser->getGT();
    short tgts=0;
    
    if(tgt[0]=='1') tgts+=2;
    if(tgt[2]=='1') tgts+=1;
    if(tgt[1]=='|') tgts+=4;
    vcf_gt.push_back(tgts);
    vcf_flag.push_back(2);
    n_placed++;
    prev_chr=chr;
  }
  if(!vcf_position.empty()) writeTreeForVcf(prev_chr,vcf_position,vcf_ref,vcf_alt,vcf_qual,vcf_nref,vcf_nalt,vcf_gt,vcf_flag);
  delete parser;
  
  cout<<"Total of " <<n_placed<< " SNPs were placed."<<endl;
}

void HisMaker::writeTreeForVcf(string chr,std::vector<int> &vcf_position,std::vector<char> &vcf_ref,
                               std::vector<char> &vcf_alt, std::vector<short> &vcf_qual, std::vector<short> &vcf_nref,
                               std::vector<short> &vcf_nalt, std::vector<short> &vcf_gt, std::vector<short> &vcf_flag) {

  cout<<"Filling and saving tree for '"<< chr <<"' ..."<<endl;
  
  stringstream sd,sn;
  sn<<"snp_"<<chr;
  sd<<"snp_"<<chr<<';'<<vcf_position.size();
  int _position;
  char _ref[2]="",_alt[2]="";
  short _qual,_nref,_nalt,_gt,_flag;
  TTree *tree = new TTree(sn.str().c_str(),sd.str().c_str());
  tree->SetMaxTreeSize(20000000000); // ~20 Gb
  tree->Branch("position", &_position,"position/I");
  tree->Branch("ref", _ref,"_ref/C");
  tree->Branch("alt", _alt,"_alt/C");
  tree->Branch("qual", &_qual,"_qual/S");
  tree->Branch("nref", &_nref,"_nref/S");
  tree->Branch("nalt", &_nalt,"_nalt/S");
  tree->Branch("GT", &_gt,"_gt/S");
  tree->Branch("flag", &_flag,"_flag/S");
  for (int i = 0;i < vcf_position.size();i++) {
    _position=vcf_position[i];
    _ref[0]=vcf_ref[i];
    _alt[0]=vcf_alt[i];
    _qual=vcf_qual[i];
    _nref=vcf_nref[i];
    _nalt=vcf_nalt[i];
    _gt=vcf_gt[i];
    _flag=vcf_flag[i];
    tree->Fill();
  }
  tree->Write(sn.str().c_str(),TObject::kOverwrite);
  delete tree;
  cout<<"Saving tree with '"<< vcf_position.size() <<"' SNPs."<<endl;
}


void HisMaker::IdVar(string *user_chroms,int n_chroms,string *user_files,int n_files,bool rmchr,bool addchr)
{
  string one_string[1] = {""};
  if (user_chroms == NULL) n_chroms = 0;
  if (user_files  == NULL || n_files == 0) {
    n_files    = 1;
    user_files = one_string;
  }
  std::vector<int>  vcf_position;
  std::map<int,char> vcf_refm;
  std::map<int,char> vcf_altm;
  long n_placed = 0;
  if (user_files[0].length() > 0)
    cout<<"Parsing file "<<user_files[0]<<" ..."<<endl;
  else
    cout<<"Parsing stdin ..."<<endl;
  VcfParser *parser = new VcfParser(user_files[0]);
  string prev_chr("");
  while (parser->parseRecord(true)) if(parser->getFilter()=="PASS") {
    string chr=parser->getChromosome();
    if(addchr) chr=Genome::extendedChromName(chr);
    if(rmchr) chr=Genome::canonicalChromName(chr);
    if (n_chroms > 0 && findIndex(user_chroms,n_chroms,chr) < 0) continue;
    if(chr!=prev_chr) {
      if(!vcf_position.empty()) updateTreeIdVar(prev_chr,vcf_position,vcf_refm,vcf_altm);
      vcf_position.clear();
      vcf_refm.clear();
      vcf_altm.clear();
    }
    int pos=parser->getPosition();
    string tref=parser->getRef();
    string talt=parser->getAlt();
    if(tref.length()!=1 && tref.length()!=2) {
      continue;
    }
    if(talt.length()!=1 && talt.length()!=2) {
      continue;
    }
    if(tref.length()==2 && talt.length()==2) {
      continue;
    }
    vcf_position.push_back(pos);
    char ttref=tref[0];
    char ttalt=talt[0];
    if(tref.length()==2) {
      ttref=tref[1];
      ttalt='-';
    }
    if(talt.length()==2) {
      ttalt=talt[1];
      ttref='-';
    }
    vcf_refm[pos]=ttref;
    vcf_altm[pos]=ttalt;
    n_placed++;
    prev_chr=chr;
  }
  if(!vcf_position.empty()) updateTreeIdVar(prev_chr,vcf_position,vcf_refm,vcf_altm);
  delete parser;
  
  cout<<"Total of " <<n_placed<< " SNPs found in vcf file."<<endl;
}

void HisMaker::updateTreeIdVar(string chr,std::vector<int> &vcf_position, std::map<int,char> &vcf_refm, std::map<int,char> &vcf_altm)
{
  cout<<"Updating tree for '"<<chr<<"' ..."<<endl;

  stringstream sd,sn;
  sn<<"snp_"<<chr;
  sd<<"snp_"<<chr<<';'<<vcf_position.size();
  
  int npass=0,ne=0;
  int _position;
  char _ref[2]="",_alt[2]="";
  short _qual,_nref,_nalt,_gt,_flag;
  io.file.cd();
  if(io.file.GetListOfKeys()->Contains(sn.str().c_str())) {
    TTree *oldtree = (TTree*) io.file.Get(sn.str().c_str());
    cout << "Found tree in root file." << endl;
    
    TTree *tree = new TTree(sn.str().c_str(),sd.str().c_str());
    tree->SetMaxTreeSize(20000000000); // ~20 Gb
    tree->Branch("position", &_position,"position/I");
    tree->Branch("ref", _ref,"_ref/C");
    tree->Branch("alt", _alt,"_alt/C");
    tree->Branch("qual", &_qual,"_qual/S");
    tree->Branch("nref", &_nref,"_nref/S");
    tree->Branch("nalt", &_nalt,"_nalt/S");
    tree->Branch("GT", &_gt,"_gt/S");
    tree->Branch("flag", &_flag,"_flag/S");
    oldtree->SetBranchAddress("position", &_position);
    oldtree->SetBranchAddress("ref", _ref);
    oldtree->SetBranchAddress("alt", _alt);
    oldtree->SetBranchAddress("qual", &_qual);
    oldtree->SetBranchAddress("nref", &_nref);
    oldtree->SetBranchAddress("nalt", &_nalt);
    oldtree->SetBranchAddress("GT", &_gt);
    oldtree->SetBranchAddress("flag", &_flag);
    
    ne=oldtree->GetEntries();
    for(int i=0;i<ne;i++) {
      oldtree->GetEntry(i);
      if(vcf_refm.find(_position) == vcf_refm.end()) {
        _flag&=~1;
        
      } else {
        if(vcf_refm[_position]==_ref[0] && vcf_altm[_position]==_alt[0]) {
          _flag|=1;
          npass++;
        } else _flag&=~1;
      }
      tree->Fill();
    }
    
    // Writing the tree
    io.file.cd();
    tree->Write(sn.str().c_str(),TObject::kOverwrite);
    
    // Deleting the tree
    delete tree;
  }
  cout<<"Saving tree with marked "<< npass <<" SNPs out of " << ne << " found in VCF file."<<endl;
}



void HisMaker::MaskVar(string *user_chroms,int n_chroms,string *user_files,int n_files,bool rmchr,bool addchr)
{
  if (user_chroms == NULL && n_chroms != 0) {
    cerr<<"No chromosome names given."<< endl <<"Aborting!"<<endl;
    return;
  }

  string chrom_names[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = getChromNamesWithTree(chrom_names,static_cast<string>(root_file_name),true);
    user_chroms = chrom_names;
  }
  
  for (int f = 0;f < n_files;f++) if (user_files[f].length() > 0) {
    cout<<"Parsing file "<<user_files[f]<<" ..."<<endl;
    string fastafile=user_files[f];
    FastaParser faparser(fastafile);
    while (faparser.nextChromosome()) {
      string name = faparser.getName();
      if(addchr) name=Genome::extendedChromName(name);
      if(rmchr) name=Genome::canonicalChromName(name);
      int c=0;
      for(;(user_chroms[c]!=name)&&(c<n_chroms);c++);
      if(c>=n_chroms) continue;
      string cseq=faparser.getData();

      cout<<"Updating tree for '"<<name<<"' chromosome..."<<endl;

      stringstream sd,sn;
      sn<<"snp_"<<name;
      
      int _position;
      char _ref[2]="",_alt[2]="";
      short _qual,_nref,_nalt,_gt,_flag;
      io.file.cd();
      if(io.file.GetListOfKeys()->Contains(sn.str().c_str())) {
        TTree *oldtree = (TTree*) io.file.Get(sn.str().c_str());
        cout << "Found tree in root file." << endl;

        TTree *tree = new TTree(sn.str().c_str(),sn.str().c_str());
        tree->SetMaxTreeSize(20000000000); // ~20 Gb
        tree->Branch("position", &_position,"position/I");
        tree->Branch("ref", _ref,"_ref/C");
        tree->Branch("alt", _alt,"_alt/C");
        tree->Branch("qual", &_qual,"_qual/S");
        tree->Branch("nref", &_nref,"_nref/S");
        tree->Branch("nalt", &_nalt,"_nalt/S");
        tree->Branch("GT", &_gt,"_gt/S");
        tree->Branch("flag", &_flag,"_flag/S");
        oldtree->SetBranchAddress("position", &_position);
        oldtree->SetBranchAddress("ref", _ref);
        oldtree->SetBranchAddress("alt", _alt);
        oldtree->SetBranchAddress("qual", &_qual);
        oldtree->SetBranchAddress("nref", &_nref);
        oldtree->SetBranchAddress("nalt", &_nalt);
        oldtree->SetBranchAddress("GT", &_gt);
        oldtree->SetBranchAddress("flag", &_flag);
        

        int ne=oldtree->GetEntries();
        for(int i=0;i<ne;i++) {
          oldtree->GetEntry(i);
          if(cseq[_position-1]=='P') _flag|=2;
          else _flag&=~2;
          tree->Fill();
        }
        
        // Writing the tree
        io.file.cd();
        tree->Write(sn.str().c_str(),TObject::kOverwrite);
      
        // Deleting the tree
        delete tree;
      }
    }
  }
}

double HisMaker::beta_pdf(double x, int a, int b) {
  return ROOT::Math::beta_pdf(x,a,b);
}

void HisMaker::produceBAF(string *user_chroms,int n_chroms,bool useGCcorr,
                          bool useHaplotype,bool useid,bool usemask,int res)
{
 
  unsigned int snpflag=(usemask?FLAG_USEMASK:0)|(useid?FLAG_USEID:0)|(useHaplotype?FLAG_USEHAP:0);
  unsigned int rdflag=useGCcorr?FLAG_GC_CORR:0;
  
  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = io.getChromNamesWithTree(chrom_names,true);
    user_chroms = chrom_names;
  }
  
  TH1 *dist_count = io.newDistributionTH1("SNPs per bin distribution (autosomes)",bin_size,"SNP count dist",snpflag,1000,0,1000);
  TH1 *dist_count_sex = io.newDistributionTH1("SNPs per bin distribution (sex chromosomes)",bin_size,"SNP count dist",snpflag|FLAG_SEX,1000,0,1000);
  TH1 *dist_baf = io.newDistributionTH1("BAF distribution (autosomes)",bin_size,"SNP baf dist",snpflag,101,0,1);
  TH1 *dist_baf_sex = io.newDistributionTH1("BAF distribution (sex chromosomes)",bin_size,"SNP baf dist",snpflag|FLAG_SEX,101,0,1);
  TH1 *dist_maf = io.newDistributionTH1("MAF distribution (autosomes)",bin_size,"SNP maf dist",snpflag,101,0,1);
  TH1 *dist_maf_sex = io.newDistributionTH1("MAF distribution (sex chromosomes)",bin_size,"SNP maf dist",snpflag|FLAG_SEX,101,0,1);
  TH1 *dist_likelihood = io.newDistributionTH1("Likelihood (autosomes)",bin_size,"SNP likelihood dist",snpflag,res,0,1);
  TH1 *dist_likelihood_sex = io.newDistributionTH1("Likelihood (sex chromosomes)",bin_size,"SNP likelihood dist",snpflag|FLAG_SEX,res,0,1);
  TH1 *dist_i1 = io.newDistributionTH1("I1 (autosomes)",bin_size,"SNP i1 dist",snpflag,101,0,1);
  TH1 *dist_i1_sex = io.newDistributionTH1("I1 (sex chromosomes)",bin_size,"SNP i1 dist",snpflag|FLAG_SEX,101,0,1);
  TH1 *dist_i2 = io.newDistributionTH1("I2 (autosomes)",bin_size,"SNP i2 dist",snpflag,101,0,1);
  TH1 *dist_i2_sex = io.newDistributionTH1("I2 (sex chromosomes)",bin_size,"SNP i2 dist",snpflag|FLAG_SEX,101,0,1);
  TH1 *dist_i3 = io.newDistributionTH1("I3 (autosomes)",bin_size,"SNP i3 dist",snpflag,101,0,1);
  TH1 *dist_i3_sex = io.newDistributionTH1("I3 (sex chromosomes)",bin_size,"SNP i3 dist",snpflag|FLAG_SEX,101,0,1);
  TH1 *dist_i4 = io.newDistributionTH1("I4 (autosomes)",bin_size,"SNP i4 dist",snpflag,101,0,1);
  TH1 *dist_i4_sex = io.newDistributionTH1("I4 (sex chromosomes)",bin_size,"SNP i4 dist",snpflag|FLAG_SEX,101,0,1);

  double glf[res],glfx[res];
  for(int i=0;i<res;i++) {
    glf[i]=1;
    glfx[i]=1;
  }

  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    chrom_lens[c] = io.lenChromWithTree(chrom);
    if(io.lenChromWithTree(chrom,true)<0) {
      cout << "There is no VAR tree for chromosome: " << chrom << ". Skiping ..." <<endl;
      continue;
    }
    TTree *tree = io.getTree(chrom,"SNP");
    int org_len = chrom_lens[c];
    if(org_len<0) {
      cout << "There is no RD tree for chromosome: " << chrom << ". Using last SNP postition for chromosome length ..." <<endl;
      org_len=tree->GetMaximum("position");
      cout<<org_len<<endl;
    }
    cout << "Calculating BAF histograms with bin size of " << bin_size << " for '" << chrom << "' ..." << endl;

    
    if (org_len <= 0) continue;
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    
    TH1 *his_count=io.newSignalTH1("SNP count",chrom,bin_size,"SNP count",snpflag,n_bins,0,len);
    TH1 *his_baf=io.newSignalTH1("BAF signal",chrom,bin_size,"SNP baf",snpflag,n_bins,0,len);
    TH1 *his_maf=io.newSignalTH1("MAF signal",chrom,bin_size,"SNP maf",snpflag,n_bins,0,len);
    TH2 *his_likelihood=io.newSignalTH2("Likelihood signal",chrom,bin_size,"SNP likelihood",snpflag,n_bins,0,len,res,0,1);
    TH1 *his_i1=io.newSignalTH1("I1 signal",chrom,bin_size,"SNP i1",snpflag,n_bins,0,len);
    TH1 *his_i2=io.newSignalTH1("I2 signal",chrom,bin_size,"SNP i2",snpflag,n_bins,0,len);
    TH1 *his_i3=io.newSignalTH1("I3 signal",chrom,bin_size,"SNP i3",snpflag,n_bins,0,len);
    TH1 *his_i4=io.newSignalTH1("I4 signal",chrom,bin_size,"SNP i4",snpflag,n_bins,0,len);

    int position;
    short nref,nalt,qual,gt,flag;
    tree->SetBranchAddress("position", &position);
    tree->SetBranchAddress("nref", &nref);
    tree->SetBranchAddress("nalt",&nalt);
    tree->SetBranchAddress("qual",&qual);
    tree->SetBranchAddress("GT",&gt);
    tree->SetBranchAddress("flag",&flag);

    int start = 1, end = bin_size, bin = 0;
    int n_ent = tree->GetEntries(), index = 0;
    int count = 0;
    int countb = 0;
    int counth = 0;
    double alt1 = 0;
    double ref1 = 0;
    double alt2 = 0;
    double ref2 = 0;
    double baf = 0;
    double maf = 0;
    double lf[res];
    for(int i=0;i<res;i++) lf[i]=1;
    tree->GetEntry(index++);
    while (index < n_ent) {
      while (position <= end && index < n_ent) {
        if( ((gt%4)==1||(gt%4)==2) && ((nalt+nref)>0) && (!useid || (flag&1)) && (!usemask || (flag&2))) {
          count++;
          if((gt%4)==1) {
            alt1+=nalt;
            ref1+=nref;
          } else {
            alt2+=nalt;
            ref2+=nref;
          }
          double s=0,gs=0;
          if(nref<nalt) nref++;
          else if(nalt<nref) nalt++;
          
          for(int i=0;i<res;i++) {
            if(useHaplotype) {
              if((gt%4)==1) lf[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1);
              else lf[i]*=beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
            } else lf[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1)+ROOT::Math::beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
            s+=lf[i];
            if(!Genome::isSexChrom(chrom)) {
              if(useHaplotype) {
                if((gt%4)==1) glf[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1);
                else glf[i]*=beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
              } else glf[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1)+ROOT::Math::beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
              gs+=glf[i];
            } else {
              if(useHaplotype) {
                if((gt%4)==1) glfx[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1);
                else glfx[i]*=beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
              } else glfx[i]*=beta_pdf(1.*i/res+0.5/res,nalt+1,nref+1)+ROOT::Math::beta_pdf(1.*i/res+0.5/res,nref+1,nalt+1);
              gs+=glfx[i];
            }
          }
          for(int i=0;i<res;i++) lf[i]/=s;
          if(Genome::isSexChrom(chrom)) {
            for(int i=0;i<res;i++) glfx[i]/=gs;
          } else {
            for(int i=0;i<res;i++) glf[i]/=gs;
          }

          double snpmaf=(nalt<nref)?1.0*(nalt)/(nalt+nref):1.0*(nref)/(nalt+nref);
          double snpbaf=1.0*(nalt)/(nalt+nref);
          maf+=(nalt<nref)?1.0*(nalt)/(nalt+nref):1.0*(nref)/(nalt+nref);
          baf+=1.0*(nalt)/(nalt+nref);
          if(!useHaplotype) {
            if(Genome::isSexChrom(chrom)) {
              dist_baf_sex->Fill(snpbaf);
              dist_maf_sex->Fill(snpmaf);
            } else {
              dist_baf->Fill(snpbaf);
              dist_maf->Fill(snpmaf);
            }
          }
          
        }
        tree->GetEntry(index++);
      }
      
      bin++;
      if (bin < 1 || bin > n_bins) {
        cerr<<"Bin out of bounds."<<endl;
      } else {
        if(useHaplotype) {
          baf=(alt1+ref1+alt2+ref2)>0?(alt1+ref2)/(alt1+ref1+alt2+ref2):0;
          maf=(baf>0.5)?1-baf:baf;
          if(Genome::isSexChrom(chrom)) {
              dist_baf_sex->Fill(baf);
              dist_maf_sex->Fill(maf);
            } else {
              dist_baf->Fill(baf);
              dist_maf->Fill(maf);
            }
        } else if(count>0) {
          baf/=count;
          maf/=count;
        }
        if(Genome::isSexChrom(chrom)) {
          dist_count_sex->Fill(count);
        } else {
          dist_count->Fill(count);
        }
        his_count->SetBinContent(bin,count);
        his_count->SetBinError(bin,0);
        if(count>0) {
          his_baf->SetBinContent(bin,baf);
          his_baf->SetBinError(bin,0);
          his_maf->SetBinContent(bin,maf);
          his_maf->SetBinError(bin,0);
          int mi;
          double mf=0;
          double i3=0;
          double i4=0;
          double si3=0;
          for(int i=0;i<res;i++) {
            if((i<(res/2+1))&&(lf[i]>mf)) {
              mf=lf[i];
              mi=i;
            }
            int cc=(alt1+ref1+alt2+ref2)/2;
            if(cc>1) {
              i3+=lf[i]*beta_pdf(1.*i/res+0.5/res,cc+1,cc+1);
              si3+=pow(beta_pdf(1.*i/res+0.5/res,cc+1,cc+1),2);
            }
            else i3=-1;
            his_likelihood->SetBinContent(bin,i+1,lf[i]);
          }
          double i1=1.0*(res/2-mi)/(res-1);
          double i2=lf[res/2]/mf;
          if(i3<0) i3=1;
          else i3/=si3;
          his_i1->SetBinContent(bin,i1);
          his_i1->SetBinError(bin,0);
          his_i2->SetBinContent(bin,i2);
          his_i2->SetBinError(bin,0);
          his_i3->SetBinContent(bin,i3);
          his_i3->SetBinError(bin,0);
          if((alt1+ref1+alt2+ref2>100)&&(count>2)&&(i1>0.01)&&(i2<0.8)) {
            i4=i1;
            his_i4->SetBinContent(bin,i4);
            his_i4->SetBinError(bin,0);
          } else i4=0;
          if(Genome::isSexChrom(chrom)) {
            dist_i1_sex->Fill(i1);
            dist_i2_sex->Fill(i2);
            dist_i3_sex->Fill(i3);
            dist_i4_sex->Fill(i4);
          } else {
            dist_i1->Fill(i1);
            dist_i2->Fill(i2);
            dist_i3->Fill(i3);
            dist_i4->Fill(i4);
          }
        }
      }
      start += bin_size;
      end   += bin_size;
      count = 0;
      countb = 0;
      counth = 0;
      
      alt1 = 0;
      ref1 = 0;
      alt2 = 0;
      ref2 = 0;
      baf = 0;
      maf = 0;
      for(int i=0;i<res;i++) lf[i]=1;
    }
    delete tree;
    
    // Writing chromosome specific histograms
    io.writeHistogramsToBinDir(bin_size,his_count,his_baf,his_maf,his_likelihood);
    io.writeHistogramsToBinDir(bin_size,his_i1,his_i2,his_i3,his_i4);

    
    // Deleting objects
    delete his_count;
    delete his_baf;
    delete his_maf;
    delete his_likelihood;
    delete his_i1;
    delete his_i2;
    delete his_i3;
    delete his_i4;
  }
  for(int i=0;i<res;i++) {
    dist_likelihood->SetBinContent(i+1,glf[i]);
    dist_likelihood_sex->SetBinContent(i+1,glfx[i]);
  }
  io.writeHistogramsToBinDir(bin_size,dist_count,dist_baf,dist_maf,dist_likelihood);
  io.writeHistogramsToBinDir(bin_size,dist_count_sex,dist_baf_sex,dist_maf_sex,dist_likelihood_sex);
  io.writeHistogramsToBinDir(bin_size,dist_i1,dist_i2,dist_i3,dist_i1_sex,dist_i2_sex,dist_i3_sex);
  io.writeHistogramsToBinDir(bin_size,dist_i4,dist_i4_sex);
  delete dist_count;
  delete dist_baf;
  delete dist_maf;
  delete dist_likelihood;
  delete dist_i1;
  delete dist_i2;
  delete dist_i3;
  delete dist_i4;

  delete dist_count_sex;
  delete dist_baf_sex;
  delete dist_maf_sex;
  delete dist_likelihood_sex;
  delete dist_i1_sex;
  delete dist_i2_sex;
  delete dist_i3_sex;
  delete dist_i4_sex;
}

void HisMaker::callBAF(string *user_chroms,int n_chroms,bool useGCcorr, bool useHaplotype,bool useid,bool usemask)
{
  unsigned int snpflag=(usemask?FLAG_USEMASK:0)|(useid?FLAG_USEID:0)|(useHaplotype?FLAG_USEHAP:0);
  unsigned int rdflag=useGCcorr?FLAG_GC_CORR:0;
  
  double pdec=0.90;
  double pmin=1.0*bin_size/1e7;
  int minc=bin_size/10000;
  int minbs=5;
  
  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = io.getChromNamesWithTree(chrom_names,true);
    user_chroms = chrom_names;
  }

  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    chrom_lens[c] = io.lenChromWithTree(chrom);
    if(io.lenChromWithTree(chrom,true)<0) {
      cout << "There is no VAR tree for chromosome: " << chrom << ". Skiping ..." <<endl;
      continue;
    }
    TTree *tree = io.getTree(chrom,"SNP");
    int org_len = chrom_lens[c];
    if(org_len<0) {
      cout << "There is no RD tree for chromosome: " << chrom << ". Using last SNP postition for chromosome length ..." <<endl;
      org_len=tree->GetMaximum("position");
      cout<<org_len<<endl;
    }

    if (org_len <= 0) continue;
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    
    TH1 *his_bafc=io.getSignal(chrom,bin_size,"SNP count",snpflag);
    TH2 *his_likelihood=(TH2*)io.getSignal(chrom,bin_size,"SNP likelihood",snpflag);
    int nx=his_likelihood->GetXaxis()->GetNbins();
    int ny=his_likelihood->GetYaxis()->GetNbins();
    TH2 *his_likelihood_call=io.newSignalTH2("Likelihood call signal",chrom,bin_size,"SNP likelihood call",snpflag,n_bins,0,len,ny,0,1);
    for(int i=0;i<nx;i++)
      for(int j=0;j<ny;j++)
        his_likelihood_call->SetBinContent(i,j,his_likelihood->GetBinContent(i,j));
    vector<int> segstart;
    vector<int> segend;
    vector<double> pval;
    for(int i=0;i<nx;i++) {
      double s=0;
      for(int j=0;j<ny;j++) s+=his_likelihood->GetBinContent(i,j);
      if((his_bafc->GetBinContent(i)>minc) && (s!=0)) {
        segstart.push_back(i);
        segend.push_back(i);
      }
    }
    for(int i=0;(i+1)<segstart.size();i++) {
      double s=0;
      for(int j=0;j<(ny);j++) {
        double lh1=his_likelihood_call->GetBinContent(segstart[i],j);
        double lh2=his_likelihood_call->GetBinContent(segstart[i+1],j);
        s+=(lh1<lh2)?lh1:lh2;
      }
      pval.push_back(s);
    }

    while(pval.size()>0) {
      double maxp=0;
      for(int i=0;i<pval.size();i++) if(pval[i]>maxp) maxp=pval[i];
      double minp=maxp*pdec;
      if(minp<pmin) minp=pmin;
      if(maxp<pmin) break;
      int i=0;
      
      while(i<pval.size()) {
        if(pval[i]>minp) {
          double tlh[ny];
          double ss=0;
          for(int j=0;j<ny;j++) tlh[j]=0;
          for(int j=0;j<ny;j++) {
            tlh[j]=his_likelihood_call->GetBinContent(segstart[i],j)*his_likelihood_call->GetBinContent(segstart[i+1],j);
            ss+=tlh[j];
          }
          for(int j=0;j<ny;j++) tlh[j]/=ss;
          for(int ii=segstart[i];ii<=segend[i];ii++) for(int j=0;j<ny;j++) his_likelihood_call->SetBinContent(ii,j,tlh[j]);
          for(int ii=segstart[i+1];ii<=segend[i+1];ii++) for(int j=0;j<ny;j++) his_likelihood_call->SetBinContent(ii,j,tlh[j]);
          segend[i]=segend[i+1];
          segstart.erase(segstart.begin()+i+1);
          segend.erase(segend.begin()+i+1);
          pval.erase(pval.begin()+i);
          if(i!=pval.size()) {
            double s=0;
            for(int j=0;j<(ny);j++) {
              double lh1=his_likelihood_call->GetBinContent(segstart[i],j);
              double lh2=his_likelihood_call->GetBinContent(segstart[i+1],j);
              s+=(lh1<lh2)?lh1:lh2;
            }
            pval[i]=s;
          }
          if(i>0) {
            double s=0;
            for(int j=0;j<(ny);j++) {
              double lh1=his_likelihood_call->GetBinContent(segstart[i-1],j);
              double lh2=his_likelihood_call->GetBinContent(segstart[i],j);
              s+=(lh1<lh2)?lh1:lh2;
            }
            pval[i-1]=s;
          }
          i++;
        } else i++;
      }
    }

    for(int i=0;i<segstart.size();i++) if((segend[i]-segstart[i])<minbs) {
      segstart.erase(segstart.begin()+i);
      segend.erase(segend.begin()+i);
      if((i+1)<segstart.size()) {
        pval.erase(pval.begin()+i);
        if(i>0) {
          double s=0;
          for(int j=0;j<(ny);j++) {
            double lh1=his_likelihood_call->GetBinContent(segstart[i-1],j);
            double lh2=his_likelihood_call->GetBinContent(segstart[i],j);
            s+=(lh1<lh2)?lh1:lh2;
          }
          pval[i-1]=s;
        }
      } else if(i>0) pval.erase(pval.begin()+i-1);
    }

    while(pval.size()>0) {
      double maxp=0;
      for(int i=0;i<pval.size();i++) if(pval[i]>maxp) maxp=pval[i];
      double minp=maxp*pdec;
      if(minp<pmin) minp=pmin;
      if(maxp<pmin) break;
      int i=0;
      
      while(i<pval.size()) {
        if(pval[i]>minp) {
          double tlh[ny];
          double ss=0;
          for(int j=0;j<ny;j++) tlh[j]=0;
          for(int j=0;j<ny;j++) {
            tlh[j]=his_likelihood_call->GetBinContent(segstart[i],j)*his_likelihood_call->GetBinContent(segstart[i+1],j);
            ss+=tlh[j];
          }
          for(int j=0;j<ny;j++) tlh[j]/=ss;
          for(int ii=segstart[i];ii<=segend[i];ii++) for(int j=0;j<ny;j++) his_likelihood_call->SetBinContent(ii,j,tlh[j]);
          for(int ii=segstart[i+1];ii<=segend[i+1];ii++) for(int j=0;j<ny;j++) his_likelihood_call->SetBinContent(ii,j,tlh[j]);
          segend[i]=segend[i+1];
          segstart.erase(segstart.begin()+i+1);
          segend.erase(segend.begin()+i+1);
          pval.erase(pval.begin()+i);
          if(i!=pval.size()) {
            double s=0;
            for(int j=0;j<(ny);j++) {
              double lh1=his_likelihood_call->GetBinContent(segstart[i],j);
              double lh2=his_likelihood_call->GetBinContent(segstart[i+1],j);
              s+=(lh1<lh2)?lh1:lh2;
            }
            pval[i]=s;
          }
          if(i>0) {
            double s=0;
            for(int j=0;j<(ny);j++) {
              double lh1=his_likelihood_call->GetBinContent(segstart[i-1],j);
              double lh2=his_likelihood_call->GetBinContent(segstart[i],j);
              s+=(lh1<lh2)?lh1:lh2;
            }
            pval[i-1]=s;
          }
          i++;
        } else i++;
      }
    }
    
    double bg_rd=0;
    double bg_rd_sigma=0;
    TH1 *his_rd=io.getSignal(chrom,bin_size,"RD",rdflag);
    TH1 *distall    = getHistogram(getDistrName(Genome::CHRALL,bin_size,false,useGCcorr));
    if(distall) getMeanSigma(distall,bg_rd,bg_rd_sigma);
    
    for(int i=0;i<segstart.size();i++) if((segend[i]-segstart[i])>=minbs) {
      double bafm=0;
      int bafmp=ny/2+1;
      for(int j=0;j<(ny);j++) if(his_likelihood_call->GetBinContent(segstart[i],j)>bafm) {bafm=his_likelihood_call->GetBinContent(segstart[i],j);bafmp=j;}
      double rd=0;
      int rd_count=0;
      if (his_likelihood_call->GetYaxis()->GetBinCenter(bafmp)!=0.5) {
        if(his_rd) {
          for(int ii=segstart[i];ii<=segend[i];ii++) {
            rd_count++;
            rd+=his_rd->GetBinContent(ii);
          }
          rd/=rd_count;

          double cnv=rd/bg_rd;
          double baf=his_likelihood_call->GetYaxis()->GetBinCenter(bafmp);
          if((1-baf)<baf) baf=1-baf;
          double f_del=(2*baf-1)/(baf-1);
          double f_dup=(1-2*baf)/baf;
          double f_cnnloh=(1-2*baf);

          string type="cnnloh     ";
          if(rd<(0.95*bg_rd)) type="deletion   ";
          if(rd>(1.05*bg_rd)) type="duplication";

          cout << type << "\t" << chrom << ":" << segstart[i]*bin_size <<  "-" <<  segend[i]*bin_size << "\t" << setw(10) <<
          segend[i]*bin_size - segstart[i]*bin_size << "\t" << setw(10) << cnv << "\t" << setw(10) << baf << "\t" << setw(10) << f_del << "\t" << setw(10) << f_dup << "\t" << setw(10) << f_cnnloh <<
          "\t" << setw(10) << his_likelihood_call->GetBinContent(segstart[i],ny/2+1)/bafm << endl;
        } else {
          double baf=his_likelihood_call->GetYaxis()->GetBinCenter(bafmp);
          if((1-baf)<baf) baf=1-baf;
          double f_del=(2*baf-1)/(baf-1);
          double f_dup=(1-2*baf)/baf;
          double f_cnnloh=(1-2*baf);
          cout << "-" << "\t" << chrom << ":" << segstart[i]*bin_size <<  "-" <<  segend[i]*bin_size << "\t" << setw(10) <<
          segend[i]*bin_size - segstart[i]*bin_size << "\t" << setw(10) << "-" << "\t" << setw(10) << baf << "\t" << setw(10) << f_del << "\t" << setw(10) << f_dup << "\t" << setw(10) << f_cnnloh <<
          "\t" << setw(10) << his_likelihood_call->GetBinContent(segstart[i],ny/2+1)/bafm << endl;
        }
      }
    }

    // Writing chromosome specific histograms
    io.writeHistogramsToBinDir(bin_size,his_likelihood_call);

    delete his_rd;
    delete his_bafc;
    delete his_likelihood;
    delete his_likelihood_call;
  }

}


void HisMaker::producePartitionBAF(string *user_chroms,int n_chroms,bool useGCcorr,
                                   bool useHaplotype,bool useid,bool usemask,int res) {
  unsigned int snpflag=(usemask?FLAG_USEMASK:0)|(useid?FLAG_USEID:0)|(useHaplotype?FLAG_USEHAP:0);
  unsigned int rdflag=useGCcorr?FLAG_GC_CORR:0;
  string chrom_names[N_CHROM_MAX];
  int    chrom_lens[N_CHROM_MAX];
  if (n_chroms == 0 || (n_chroms == 1 && user_chroms[0] == "")) {
    n_chroms = io.getChromNamesWithTree(chrom_names,true);
    user_chroms = chrom_names;
  }
  for (int c = 0;c < n_chroms;c++) {
    string chrom = user_chroms[c];
    chrom_lens[c] = io.lenChromWithTree(chrom);
    if(chrom_lens[c]<0) {
      cout << "There is no RD tree for chromosome: " << chrom << ". Skiping ..." <<endl;
      continue;
    }
    if(io.lenChromWithTree(chrom,true)<0) {
      cout << "There is no VAR tree for chromosome: " << chrom << ". Skiping ..." <<endl;
      continue;
    }
    cout << "Calculating BAF histograms with bin size of " << bin_size << " for '" << chrom << "' ..." << endl;
    
    int org_len = chrom_lens[c];
    if (org_len <= 0) continue;
    int n_bins = org_len/bin_size + 1;
    int len = n_bins*bin_size;
    
    TH1 *partition = io.getSignal(chrom,bin_size,"RD partition",rdflag);
    TH1 *his_maf = io.getSignal(chrom,bin_size,"SNP maf",snpflag);
    int pmax = partition->GetMaximum();
    int pmin = partition->GetMinimum();
    
    TString h_name_rdbaf = io.signalName(chrom,bin_size,"SNP count",snpflag)+"_maf_rd";
    TString h_title_rdbaf = "MAF vs RD distribution "; h_title_rdbaf += chrom;
    TH2 *his_rdbaf = new TH2D(h_name_rdbaf,h_title_rdbaf,110,-0.5,1.5,1000,pmin,pmax);
    his_rdbaf->SetDirectory(0);
    
    TString h_name_mafl = io.signalName(chrom,bin_size,"SNP count",snpflag)+"_maf_levels";
    TString h_title_mafl = "MAF levels for "; h_title_mafl += chrom;
    TH1 *his_mafl = new TH1D(h_name_mafl,h_title_mafl,n_bins,0,len);
    his_mafl->SetDirectory(0);
    
    double op=-1;
    int oi;
    double sum=0;
    int count=0;
    for(int i=0;i<n_bins;i++) {
      double p=partition->GetBinContent(i+1);
      if(p!=op) {
        if(op!=-1) {
          for(int j=oi;j<i;j++) {
            his_mafl->SetBinContent(j+1,(count>0)?sum/count:-1);
            his_mafl->SetBinError(j+1,0);
          }
          oi=i;
          if(count>0) his_rdbaf->Fill(sum/count,op);
        }
        sum=0;
        count=0;
      } else {
        if(his_maf->GetBinContent(i+1)>0.15) {
          sum+=his_maf->GetBinContent(i+1);
          count++;
        }
      }
      op=p;
    }
    for(int j=oi;j<n_bins;j++) {
      his_mafl->SetBinContent(j+1,(count>0)?sum/count:-1);
      his_mafl->SetBinError(j+1,0);
    }
    if(count>0) his_rdbaf->Fill(sum/count,op);
    io.writeHistogramsToBinDir(bin_size,his_rdbaf,his_mafl);
    delete his_rdbaf;
    delete his_mafl;
  }
}


void HisMaker::writeTreeForChromosome(string chrom,short *arr_p,
				      short *arr_u,int len)
{
  stringstream ss; ss<<chrom<<';'<<len;
  string description = ss.str();

  short rd_u,rd_p;
  int position;
  TTree *tree = new TTree(chrom.c_str(),description.c_str());
  tree->SetMaxTreeSize(20000000000); // ~20 Gb
  tree->Branch("position", &position,"position/I");
  tree->Branch("rd_unique",&rd_u,    "rd_u/S");
  tree->Branch("rd_parity",&rd_p,    "rd_p/S");
  // Filling the tree
  for (int i = 0;i < len;i++) {
    //if (i%100000 == 0) cout<<i<<endl;
    rd_u = (arr_u) ? arr_u[i] : 0;
    rd_p = (arr_p) ? arr_p[i] : 0;
    if (rd_u > 0 || rd_p > 0) {
      position = i;
      tree->Fill();
    }
  }
  // Writing the tree
  tree->Write(chrom.c_str(),TObject::kOverwrite);
  // Deleting the tree
  delete tree;
}
  
bool HisMaker::readTreeForChromosome(TString fileName,string chrom,
				     short *arr_p,short *arr_u)
{
  TFile file(fileName.Data());
  if (file.IsZombie()) {
    cerr<<"Can't open/read file '"<<fileName<<"'."<<endl;
    return false;
  }
  TTree *tree = (TTree*)file.Get(chrom.c_str());
  if (!tree)
    tree = (TTree*)file.Get(Genome::canonicalChromName(chrom).c_str());
  if (!tree) {
    cerr<<"Can't find tree for '"<<chrom<<"' in file '"
	<<fileName<<"'."<<endl;
    return false;
  }
    
  int position;
  short rd_unique,rd_parity;
  tree->SetBranchAddress("position", &position);
  tree->SetBranchAddress("rd_unique",&rd_unique);
  tree->SetBranchAddress("rd_parity",&rd_parity);
  int n_ent = tree->GetEntries();
  for (int i = 0;i < n_ent;i++) {
    tree->GetEntry(i);
    if (arr_p) arr_p[position] += rd_parity;
    if (arr_u) arr_u[position] += rd_unique;
  }
  file.Close();
  return true;
}

void HisMaker::writeATTreeForChromosome(string chrom,int *arr,int n)
{
  string description = chrom; description += " AT runs";
  string name        = chrom; name        += "_at";

  int start,end;
  TTree *tree = new TTree(name.c_str(),description.c_str());
  tree->Branch("start",&start,"start/I");
  tree->Branch("end",  &end,  "end/I");

  // Filling the tree
  for (int i = 0;i < n;i += 2) {
    start = arr[i];
    end   = arr[i + 1];
    tree->Fill();
  }
    
  // Writing the tree
  tree->Write(tree->GetName(),TObject::kOverwrite);
  // Deleting the tree
  delete tree;
}

TString HisMaker::getDirName(int bin)
{
  TString ret = "bin_";
  ret += bin;
  return ret;
}

TString HisMaker::getDistrName(string chr,int bin,
			       bool useATcorr,bool useGCcorr)
{
  TString ret = "rd_p_";
  if (Genome::isSexChrom(chr)) ret += "xy_";
  if (useATcorr)  ret += "AT_";
  if (useGCcorr)  ret += "GC_";
  ret += bin;
  return ret;
}

TString HisMaker::getUDistrName(string chr,int bin)
{
  TString ret = "rd_u_";
  if (Genome::isSexChrom(chr)) ret += "xy_";
  ret += bin;
  return ret;
}

TString HisMaker::getRawSignalName(TString chr,int bin)
{
  TString ret = getSignalName(chr,bin,false,false);
  ret += "_raw";
  return ret;
}


TString HisMaker::getSignalName(TString chr,int bin,
				bool useATcorr,bool useGCcorr)
{
  TString ret = "his_rd_p_" + chr + "_";
  ret += bin;
  if (useATcorr) ret += "_AT";
  if (useGCcorr) ret += "_GC";
  return ret;

}
TString HisMaker::getAIBName(TString chr,int bin)
{
  TString ret = "his_aib_" + chr + "_";
  ret += bin;
  return ret;
}

TString HisMaker::getPartitionName(TString chr,int bin,
				   bool useATcorr,bool useGCcorr)
{
  TString ret = getSignalName(chr,bin,false,false) + "_partition";
  if (useATcorr) ret += "_AT";
  if (useGCcorr) ret += "_GC";
  return ret;
}

TString HisMaker::getUSignalName(TString chrom,int bin)
{
  TString ret = "his_rd_u_" + chrom + "_";
  ret += bin;
  return ret;
}

TString HisMaker::getGCName(TString chrom,int bin)
{
  TString ret = chrom + "_gc_";
  ret += bin;
  return ret;
}

int HisMaker::readChromosome(string chrom,char *seq,int max_len)
{
  seq[0]   = '\0';
  max_len += 2;

  int ret = 0;
  string chrom_file = dir_ + "/" + chrom + ".fa";
  ifstream file(chrom_file.c_str());
  if (!file.is_open()) {
    chrom_file = dir_ + "/" + Genome::extendedChromName(chrom) + ".fa";
    file.open(chrom_file.c_str());
    if (!file.is_open()) {
      cerr<<"Can't open file with chromosome sequence."<<endl;
      cerr<<"No chromosome/contig information parsed."<<endl;
      return ret;
    }
  }
  while (file.good() && *seq != '>') file.getline(seq,1024);
  if (!file.good()) {
    cerr<<"Can't find sequence in file '"<<chrom_file<<"'."<<endl;
    cerr<<"No chromosome/contig information parsed."<<endl;
    return ret;
  }
  while (file.good()) {
    file.getline(seq,max_len);
    while (*seq++ != '\0' && max_len-- > 0) ret++;
    seq--;
  }
  file.close();

  return ret;
}
