// c++ main.cpp -lopencv_highgui -lopencv_imgproc -lopencv_core -lopencv_imgcodecs -I/usr/local/include/opencv4 -std=c++11

#include <iostream>

#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

using namespace std;

string REF_PATH = "";
string SHEET_PATH = "";
string MARKS_PATH = "./output/marks.xml";
string RENDER_PATH = "./output/render.png";

int ACTION = 0;
int DISPLAY_WIP = 1;
int MAX_ITER = 2000;
int VAR_ITER = 1000;
int UNDERPAINT_ITER = 500;
int MAX_K = 200;
int MARK_W_MIN = 5;
int MARK_W_MAX = 120;
int MARK_W_VAR = 20;
int REPAINT_SCALE = 1;
int SPRITE_W = 128;
int SPRITE_N = 1564;
int ADAPTIVE_K = 1;
int ROTATE_MARKS = 1;
int GAUSS_KERN = 0;
int REMAP_COLOR = 1;

struct mark_t {
  int i;
  int x;
  int y;
  int w;
  float r;
};

vector<mark_t> history;

cv::Mat sheet_rgb;
cv::Mat sheet_a;
cv::Mat target;
cv::Mat canvas;
cv::Mat flip;
cv::Mat canvas_roi;
cv::Mat feature;

vector<float> prob_dist;
float prob_dist_sum = 0;

int inner_w = 0;
int inner_h = 0;

int canvas_w = 0;
int canvas_h = 0;
int canvas_pad = MARK_W_MAX + MARK_W_VAR;

cv::Scalar bg_color = cv::Scalar(.5f,.5f,.5f);

float map_clamp(float value,float istart,float istop,float ostart,float ostop){
  float omin = min(ostart,ostop);
  float omax = max(ostart,ostop);
  return min(max(ostart + (ostop - ostart) * ((value - istart)*1.0f / (istop - istart)), omin), omax);
}

int bin_search(std::vector<float>& arr, int i0, int i1, float x){
  if (i1 - i0 <= 1){
    return (arr[i0] <= x && x < arr[i1]) ? i0 : -1;
  }
  int m = (i0 + i1)/2;
  if (arr[m] <= x){
    return bin_search(arr,m,i1,x);
  }else{
    return bin_search(arr,i0,m,x);
  }
}

void write_history(){
  cv::FileStorage fs(MARKS_PATH, cv::FileStorage::WRITE);
  fs << "size" << "{:" << "w" << inner_w << "h" << inner_h << "}";
  fs << "marks" << "[";
  for( int i = 0; i < history.size(); i++ ){
    mark_t m = history[i];
    fs << "{:" << "i" << m.i << "x" << m.x << "y" << m.y << "w" << m.w << "r" << (int)m.r << "}";
  }
  fs << "]";
  fs.release();
}

void read_history(float scale){
  cv::FileStorage fs(MARKS_PATH, cv::FileStorage::READ);
  
  inner_w = (int)((float)((int)fs["size"]["w"]) * scale);
  inner_h = (int)((float)((int)fs["size"]["h"]) * scale);
  canvas_pad = (int)((float)canvas_pad * scale);
  canvas_w = inner_w+canvas_pad*2;
  canvas_h = inner_h+canvas_pad*2;
  
  cv::FileNode features = fs["marks"];
  for(cv::FileNodeIterator it = features.begin() ; it != features.end(); it++){
    mark_t m;
    m.i = (*it)["i"];
    m.x = (int)((float)(*it)["x"]*scale);
    m.y = (int)((float)(*it)["y"]*scale);
    m.w = (int)((float)(*it)["w"]*scale);
    m.r = (*it)["r"];
    history.push_back(m);
  }
  fs.release();
}

void write_canvas(){
  cv::Mat c255;
  cv::Mat c1;
  c1 = canvas_roi * 255;
  c1.convertTo(c255, CV_8UC3);
  cv::imwrite(RENDER_PATH,c255);
}

cv::Rect index2rect(int idx){
  int w = sheet_rgb.size().width;
  int h = sheet_rgb.size().height;
  int cols = w / SPRITE_W;
  int c = idx % cols;
  int r = (int)(idx / cols);
  return cv::Rect(c*SPRITE_W,r*SPRITE_W,SPRITE_W,SPRITE_W);
}

double loss(cv::Mat& mat0, cv::Mat& mat1, cv::Rect rect){
  cv::Mat d = mat0(rect)-mat1(rect);
  cv::Mat d2;
  cv::multiply(d,d,d2);
  cv::Scalar s = cv::sum(d2);
  return (s[0] + s[1] + s[2])/rect.area();
}

cv::Point sample_prob_dist(){
  float k = ( prob_dist_sum * ((float)rand()/(float)RAND_MAX) );
  int i = bin_search(prob_dist,0,prob_dist.size()-1,k);
  int x = i % (inner_w);
  int y = i / (inner_w);
  return cv::Point(x,y);
}

cv::Rect slap(cv::Mat& src, mark_t& m){
  int w = m.w;
  int x = m.x + canvas_pad;
  int y = m.y + canvas_pad;
  if (w % 2 == 1){w++;}
  
  cv::Rect rect_src;
  cv::Rect rect;
  cv::Mat fg;
  cv::Mat bg;
  cv::Mat mask;
  
  if (ROTATE_MARKS){
    int pad = (GAUSS_KERN)? ((int)(w*0.2)) : 0; // sqrt(2)/2-0.5
    int fw = w+pad*2;
    
    rect_src = cv::Rect(x-fw/2,y-fw/2,fw,fw);

    bg = src(rect_src);

    rect = index2rect(m.i);

    mask = sheet_a(rect); cv::resize(mask,mask,cv::Size(w,w));
    fg = sheet_rgb(rect); cv::resize(fg,fg,cv::Size(w,w));

    cv::Point2f pc(w/2, w/2);
    cv::Mat rot = cv::getRotationMatrix2D(pc, m.r, 1.0);
    rot.at<double>(0,2) += pad;
    rot.at<double>(1,2) += pad;

    cv::warpAffine(fg, fg, rot, cv::Size(fw,fw));
    cv::warpAffine(mask, mask, rot, cv::Size(fw,fw));
    
  }else{
    rect_src = cv::Rect(x-w/2,y-w/2,w,w);
    bg = src(rect_src);
    rect = index2rect(m.i);
    mask = sheet_a(rect); cv::resize(mask,mask,cv::Size(w,w));
    fg = sheet_rgb(rect); cv::resize(fg,fg,cv::Size(w,w));
  }
  cv::multiply(cv::Scalar::all(1.0)-mask,bg,bg);
  cv::add(bg,fg,bg);

  return rect_src;
}

void underpaint(){
  for (int i = 0; i < UNDERPAINT_ITER; i++){
    mark_t m;
    m.i = rand() % SPRITE_N;
    m.x = rand() % inner_w;
    m.y = rand() % inner_h;
    m.w = MARK_W_MAX;
    m.r = (ROTATE_MARKS) ? (rand() % 360) : 0;
    slap(canvas, m);
    history.push_back(m);
  }
}

mark_t paint_iter(int k, int min_w, int max_w){
  vector<mark_t> candidates;
  double m_v = -DBL_MAX;
  int m_i = 0;
  for (int i = 0; i < k; i++){
    cv::Point pt = sample_prob_dist();
    
    mark_t m;
    m.i = rand() % SPRITE_N;
    m.x = pt.x;
    m.y = pt.y;
    m.w = min_w + rand() % (max_w-min_w);
    m.r = rand() % 360;

    canvas.copyTo(flip(cv::Rect(0,0,canvas_w,canvas_h)));

    cv::Rect rect = slap(flip, m);
    double old_d = loss(canvas, target, rect);
    double new_d = loss(flip, target, rect);

    double improve = old_d - new_d;

    if (improve > m_v){
      m_v = improve;
      m_i = i;
    }
    candidates.push_back(m);
  }
  slap(canvas, candidates[m_i]);
  return candidates[m_i];
}

void paint(){
  for (int i = 0; i < MAX_ITER; i++){
    int k = (ADAPTIVE_K) ? ((int)map_clamp(i, 0, (float)MAX_ITER*0.9, (float)MAX_K*0.1, MAX_K)) : MAX_K;
    int min_w = map_clamp(i, 0, VAR_ITER, MARK_W_MAX-MARK_W_VAR, MARK_W_MIN);
    int max_w = min_w + MARK_W_VAR;
    
    mark_t m = paint_iter(k,min_w,max_w);
    history.push_back(m);
    if (i % 5 == 0){
      printf("iter=%5d/%5d k=%3d/%3d w=(%3d~%3d)\n",i,MAX_ITER,k,MAX_K,min_w,max_w);
      if (DISPLAY_WIP){
        cv::imshow( " ", canvas_roi );
        cv::waitKey(1);
      }
    }
    if (i % 100 == 0 || i == MAX_ITER - 1){
      cout << "writing to file..." << endl;
      write_history();
      write_canvas();
    }
    if (i % 100 == 0 || i == MAX_ITER - 1){
      cout << "loss: " << loss(canvas, target, cv::Rect(canvas_pad,canvas_pad,inner_w,inner_h)) << endl;
    }
  }
}

void repaint(){
  int i = 0;
  while (true){
    for (int j = 0; j < 100; j++){
      slap(canvas,history[i]);
      if (i == history.size()-1){
        write_canvas();
      }
      if (i < history.size()-1) {i++;}else{return;}
    }
    cv::imshow(" ", canvas_roi);
    cv::waitKey(1);
  }
}

void callibrate_target_color(){
  cv::Mat mask;
  sheet_a.convertTo(mask, CV_8UC3);
  cv::cvtColor(mask,mask,cv::COLOR_RGB2GRAY);
  
  cv::Scalar m_s,d_s;
  cv::meanStdDev(sheet_rgb,m_s,d_s,mask);
  
  cv::Scalar m_t,d_t;
  cv::Mat target_roi = target(cv::Rect(canvas_pad,canvas_pad,inner_w,inner_h));
  cv::meanStdDev(target_roi,m_t,d_t);
  
  cv::Scalar m_diff = m_s - m_t;
  cv::Scalar d_scl = cv::Scalar(d_s[0]/d_t[0], d_s[1]/d_t[1], d_s[2]/d_t[2]);
  target_roi -= m_t;
  cv::multiply(target_roi, d_scl, target_roi);
  target_roi += m_s;
  
  if (DISPLAY_WIP){
    for (int i = 0; i < 50; i++){
      cv::imshow(" ", target_roi);
      cv::waitKey(1);
    }
  }
}

void init_sheet(cv::Mat& sheet_raw){

  if (sheet_raw.channels() == 3){
    cv::cvtColor(sheet_raw, sheet_raw, cv::COLOR_RGB2RGBA);
  }
  if (sheet_raw.channels() == 1){
    cv::cvtColor(sheet_raw, sheet_raw, cv::COLOR_GRAY2RGBA);
  }

  vector<cv::Mat> sheet_ch(3);
  cv::split(sheet_raw, sheet_ch);
  
  sheet_a = sheet_ch[3];
  
  sheet_ch.pop_back();
  cv::merge(sheet_ch,sheet_rgb);
  
  sheet_a.convertTo(sheet_a, CV_32FC1); // *C1*
  sheet_rgb.convertTo(sheet_rgb, CV_32FC3);
  
  sheet_a/=255.f;
  sheet_rgb/=255.f;
  
  if (GAUSS_KERN){
    cv::Mat gauss_x = cv::getGaussianKernel(255, (GAUSS_KERN == 2) ? 18.f : 36.f, CV_32FC1)*100;
    cv::Mat gauss_y = cv::getGaussianKernel(255, 36.f, CV_32FC1)*100;
    cv::Mat kern = gauss_x * gauss_y.t();
    cv::Mat white = cv::Mat(kern.size(), CV_32FC1);
    white.setTo(cv::Scalar(0.05f));
    kern = cv::min(kern,white);
    cv::normalize(kern, kern, 0.f, 1.f, cv::NORM_MINMAX, -1);
    cv::resize(kern,kern,cv::Size(SPRITE_W,SPRITE_W));
    
//    while (1){ cv::imshow(" ", kern);cv::waitKey(1);}
    for (int i = 0; i < SPRITE_N; i++){
      cv::Mat fg = sheet_a(index2rect(i));
      cv::multiply(kern,fg,fg);
    }
  }
  
  cv::cvtColor(sheet_a, sheet_a, cv::COLOR_GRAY2RGB);
  sheet_a.convertTo(sheet_a, CV_32FC3);
  
  cv::imwrite("test.exr",sheet_a);
  
  cv::multiply(sheet_a,sheet_rgb,sheet_rgb);
}

void init_target(cv::Mat& target_raw){
  cv::Mat tmp;
  target_raw.convertTo(tmp, CV_32FC3);
  tmp /= 255.f;
  inner_w = target_raw.size().width;
  inner_h = target_raw.size().height;
  canvas_w = inner_w+canvas_pad*2;
  canvas_h = inner_h+canvas_pad*2;
  target = cv::Mat(cv::Size(canvas_w,canvas_h), CV_32FC3);
  target.setTo(bg_color);
  tmp.copyTo(target(cv::Rect(canvas_pad,canvas_pad,inner_w,inner_h)));
}


void init_canvas(){
  canvas = cv::Mat(cv::Size(canvas_w,canvas_h), CV_32FC3);
  flip = cv::Mat(cv::Size(canvas_w,canvas_h), CV_32FC3);
  canvas.setTo(bg_color);
  flip.setTo(bg_color);
  canvas_roi = canvas(cv::Rect(canvas_pad,canvas_pad,inner_w,inner_h));
}

void init_prob_dist(cv::Mat& target_raw){
  cv::Mat grad_x;
  cv::Mat grad_y;
  cv::cvtColor(target_raw, feature, cv::COLOR_RGB2GRAY);
  
  cv::GaussianBlur( feature, feature, cv::Size(21,21), 0, 0, cv::BORDER_DEFAULT );

  cv::Sobel(feature, grad_x, CV_16S, 1, 0);
  cv::Sobel(feature, grad_y, CV_16S, 0, 1);
  cv::convertScaleAbs( grad_x, grad_x );
  cv::convertScaleAbs( grad_y, grad_y );
  cv::addWeighted( grad_x, 0.5, grad_y, 0.5, 0, feature );
  feature.convertTo(feature, CV_32FC3);
  feature /= 255.f;
  cv::GaussianBlur( feature, feature, cv::Size(51,51), 0, 0, cv::BORDER_DEFAULT );
  cv::normalize(feature, feature, 0.2f, 1.f, cv::NORM_MINMAX, -1);

  inner_w = target_raw.size().width;
  inner_h = target_raw.size().height;

  prob_dist_sum = 0;
  for (int y = 0; y < inner_h; y++){
    for (int x = 0; x < inner_w; x++){
      float prob = feature.at<float>(y,x);
      prob_dist.push_back(prob_dist_sum);
      prob_dist_sum += prob;
    }
  }
//  while (1){ cv::imshow(" ", feature);cv::waitKey(1);}
}



void read_settings(string settings_path){
  cv::FileStorage fs(settings_path, cv::FileStorage::READ);
  REF_PATH = (string)fs["ref_path"];
  SHEET_PATH = (string)fs["sheet_path"];
  MARKS_PATH = (string)fs["marks_path"];
  RENDER_PATH = (string)fs["render_path"];
  
  ACTION   = fs["action"];
  MAX_ITER = fs["max_iter"];
  VAR_ITER = fs["var_iter"];
  UNDERPAINT_ITER = fs["underpaint_iter"];
  MAX_K      = fs["max_k"];
  MARK_W_MIN = fs["mark_w_min"];
  MARK_W_MAX = fs["mark_w_max"];
  MARK_W_VAR = fs["mark_w_var"];
  REPAINT_SCALE = fs["repaint_scale"];
  SPRITE_N = fs["sprite_n"];
  SPRITE_W = fs["sprite_w"];
  DISPLAY_WIP = fs["display_wip"];
  ADAPTIVE_K = fs["adaptive_k"];
  ROTATE_MARKS = fs["rotate_marks"];
  GAUSS_KERN = fs["gauss_kern"];
  REMAP_COLOR = fs["remap_color"];
  fs.release();
}



int main(int argc, char *argv[]){
  
  if (argc > 1){
    cout << argv[1] << endl;
    read_settings(argv[1]);
    cout << "settings file loaded." << endl;
  }
  
  cv::Mat sheet_raw = cv::imread(SHEET_PATH, cv::IMREAD_UNCHANGED);
  cout << sheet_raw.size() << "x" << sheet_raw.channels() << endl;
  init_sheet(sheet_raw);
  
  cv::Mat target_raw = cv::imread(REF_PATH, cv::IMREAD_COLOR);
  cout << target_raw.size() << "x" << target_raw.channels() << endl;
  
  
  if (ACTION == 0){
  
    init_target(target_raw);
    init_prob_dist(target_raw);
    
    if (REMAP_COLOR){
      callibrate_target_color();
    }
    init_canvas();
    
    underpaint();
    paint();
    
  }else if (ACTION == 1){
    
    read_history(REPAINT_SCALE);
    init_canvas();
    repaint();
  }
  return 0;
}
