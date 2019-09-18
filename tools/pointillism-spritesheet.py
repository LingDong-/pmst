import cv2; cv = cv2
import numpy as np

def hsv2rgb(h,s,v):
    t = np.array([[[h,s,v]]]).astype(np.uint8)
    return [int(x) for x in cv.cvtColor(t,cv.COLOR_HSV2BGR)[0,0]]

colors = []
for h in range(0,30,2)+range(100,130,2):
  for s in range(128,255,10):
    for v in range(33,255,10):
      v = 0.1*255+0.9*v
      [r,g,b] = hsv2rgb(h,s,v)
      colors.append((r,g,b))

w = 115
h = 78
u = 10
v = 4
W = w*u
H = h*u

im = np.ones((H,W,4),np.uint8)*255
a = np.zeros((H,W),np.uint8)

for i in range(h):
  for j in range(w):
    x = int((j+0.5)*u)
    y = int((i+0.5)*u)
    cv.circle(im,(x,y),v,colors[i*w+j],-1)
    cv.circle(a,(x,y),v,255,-1)

im[:,:,3]=a
cv.imwrite('8970x10.png',im)