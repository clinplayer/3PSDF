#include "computeLocal3PoleSDF.h"

#include <math.h> 

#include <random>
#include <queue>
#include <set>
#include <stack>
#include <deque>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <chrono> 
#include <iostream>

#include "utilities.h"
#include "distComputeUtility.h"
#include "glm/gtc/matrix_transform.hpp"
#include "manifold/Intersection.h"
#include <igl/readOBJ.h>
#include <igl/writePLY.h>
#include <igl/copyleft/marching_cubes.h>
#include <igl/signed_distance.h>
#include <igl/read_triangle_mesh.h>
#include <igl/parula.h>
#include <igl/point_mesh_squared_distance.h>
#include "igl/AABB.h"
#include "igl/WindingNumberAABB.h"

#define PI 3.14159265

// for debug only!
#define FOR_DEBUG 1

using namespace igl;
using namespace std::chrono; 

// given point p and triangle (a,b,c), return p's barycentric coordinate
void Barycentric(const Vector3d& p, const Vector3d& a, const Vector3d& b, const Vector3d& c, float &u, float &v, float &w)
{
    Vector3d v0 = b - a, v1 = c - a, v2 = p - a;
    float d00 = v0.dot(v0); 
    float d01 = v0.dot(v1);
    float d11 = v1.dot(v1);
    float d20 = v2.dot(v0);
    float d21 = v2.dot(v1);
    float denom = d00 * d11 - d01 * d01;
    v = (d11 * d20 - d01 * d21) / denom;
    w = (d00 * d21 - d01 * d20) / denom;
    u = 1.0f - v - w;
}

string vec2String(const RowVector3d& v){
	ostringstream strs;
	strs << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
	return strs.str();
}

typedef pair<int,int> CEdge;

// Compute sign of the query point via angle weighted pseudonormals
double computeSignViaPseudonormal(
        const int& vertID,
        const MatrixXd& all_verts,
        const MatrixXi& all_faces,
        const Vector3d& dir, 
        const MatrixXd& face_normals,
        map<int, vector<int>>& vert2Face) {
  auto iter = vert2Face.find(vertID);
  if (iter == vert2Face.end()) {
      std::cout << "The vertex is not found in function verifyAngleViaVertConnection!" << std::endl;
  }

  vector<int> faces = vert2Face[vertID];
  map<int, int> fids; // use map to avoid duplicated face ids
  for(auto f : faces)
      fids[f] = 1;
  RowVector3d sumNormal(0.0, 0.0, 0.0);
  for(auto i=fids.begin(); i!=fids.end(); i++) {
    // Compute angles
    RowVector3i f_verts = all_faces.row(i->first);
    int v0 = -1;
    int v1 = -1;
    // find out the other two vertices other than the input vertex id
    for (int j = 0; j < 3; j++) {
      if (f_verts[j] == vertID)
        continue;
      if (v0 == -1) {
        v0 = f_verts[j];
        continue;
      }
      if (v1 == -1) {
        v1 = f_verts[j];
      }
    }
    RowVector3d edge0 = all_verts.row(v0) - all_verts.row(vertID);
    RowVector3d edge1 = all_verts.row(v1) - all_verts.row(vertID);
    // std::cout << "before normalization: edge0: " << std::endl << edge0 << std::endl << " edge1: " << std::endl << edge1 << std::endl;
    edge0.normalize();
    edge1.normalize();
    // std::cout << "after normalization: edge0: " << std::endl << edge0 << std::endl << " edge1: " << std::endl << edge1 << std::endl;
    double dot_p = edge0.dot(edge1);
    double angle = acos(dot_p);
    RowVector3d n = face_normals.row(i->first);
    sumNormal += angle * n;
    // std::cout << "dot p: " << dot_p << std::endl << "angle: " << angle << std::endl << "sumNormal: " << sumNormal << std::endl;
  }

  sumNormal.normalize();
  // std::cout << "normalized sum normal: " << sumNormal << std::endl;
  double sign = sumNormal.dot(dir);
  // std::cout << "dir : " << dir << std::endl << "sign: " << sign << std::endl;
  return sign;
}

double getMaxAngleViaVertConnection(
                    const int& vertID,
                    const Vector3d& dir, 
                    const MatrixXd& face_normals,
                    map<int, vector<int>>& vert2Face
                    )
{
    auto iter = vert2Face.find(vertID);
    if (iter == vert2Face.end())
    {
        std::cout << "The vertex is not found in function verifyAngleViaVertConnection!" << std::endl;
    }

    vector<int> faces = vert2Face[vertID];
    map<int, int> fids; // use map to avoid duplicated face ids
    for(auto f : faces)
        fids[f] = 1;
    double maxV = -1e10;
    RowVector3d aveNormal(0.0, 0.0, 0.0);
    for(auto i=fids.begin(); i!=fids.end(); i++)
    {
        RowVector3d n = face_normals.row(i->first);
        aveNormal += n;
        // double dotprod = dir.dot(n);
        // if (dotprod > maxV)
        //     maxV = dotprod;
    }

    aveNormal = aveNormal / double(fids.size());
    aveNormal.normalize();
    maxV = aveNormal.dot(dir);
    return maxV;
}

// input params:
// a, b, c -- the barycentric coordinate of v0, v1, v2 of a triangle
double getMaxAngleViaEdgeConnection(const float& a, const float& b, const float& c, 
                    const int& faceID,
                    const Vector3d& dir, 
                    const MatrixXi& F,
                    const MatrixXd& face_normals,
                    map<CEdge, vector<int>>& edge2Face)
{
    float eps = 1e-4;
    map<int, int> fids;
    int v0 = F.row(faceID)[0], v1 = F.row(faceID)[1], v2 = F.row(faceID)[2];
    CEdge e;
    if (abs(a) < eps){
        if (v1 < v2)
            e = CEdge(v1, v2);
        else
            e = CEdge(v2, v1);
        auto itr = edge2Face.find(e);
        if (itr == edge2Face.end())
        {
            std::cout << "The edge is not found in function getMaxAngleViaEdgeConnection!" << std::endl;
        }
        vector<int> faces = itr->second;
        for(auto f : faces)
            fids[f] = 1;
        // fids.insert(fids.end(), edge2Face[e].begin(), edge2Face[e].end());
    }
    if (abs(b) < eps){
        if (v2 < v0)
            e = CEdge(v2, v0);
        else
            e = CEdge(v0, v2);
        auto itr = edge2Face.find(e);
        if (itr == edge2Face.end())
        {
            std::cout << "The edge is not found in function getMaxAngleViaEdgeConnection!" << std::endl;
        }
        vector<int> faces = itr->second;
        for(auto f : faces)
            fids[f] = 1;
    }
    if (abs(c) < eps){
        if (v1 < v0)
            e = CEdge(v1, v0);
        else
            e = CEdge(v0, v1);
        auto itr = edge2Face.find(e);
        if (itr == edge2Face.end())
        {
            std::cout << "The edge is not found in function getMaxAngleViaEdgeConnection!" << std::endl;
        }
        vector<int> faces = itr->second;
        for(auto f : faces)
            fids[f] = 1;
    }

    double maxV = -1e10;
    RowVector3d aveNormal(0.0, 0.0, 0.0);
    
    for(auto i=fids.begin(); i!=fids.end(); i++)
    {
        RowVector3d n = face_normals.row(i->first);
        aveNormal += n;
        // double dotprod = dir.dot(n);
        // if (dotprod > maxV)
        //     maxV = dotprod;
    }

    aveNormal = aveNormal / double(fids.size());
    aveNormal.normalize();
    maxV = aveNormal.dot(dir);
    return maxV;
}

void reOrderMeshIndices(const MatrixXd& allV, const MatrixXi& allF, const vector<int>& partF, MatrixXd& outV, MatrixXi& outF)
{
    map<int, int> old2New;  // mapp from old index to new index in new mesh
    for(int i=0; i < partF.size(); ++i)
    {
        int fid = partF[i];
        old2New[allF(fid,0)] = -1;
        old2New[allF(fid,1)] = -1;
        old2New[allF(fid,2)] = -1;
    }
    // std::cout << "New mesh includes: " << old2New.size() << " vertices." << std::endl;
    // only keep selected vertices and update vertex index
    outV = MatrixXd(old2New.size(), 3);
    int cnt = 0;
    for (auto iter = old2New.begin(); iter != old2New.end(); iter++)
    {
        outV.row(cnt) = allV.row(iter->first);
        iter->second = cnt++;
    }
    // update face index
    outF = MatrixXi(partF.size(), 3);
    for(int i=0; i < partF.size(); ++i)
    {
        int fid = partF[i];
        outF(i,0) = old2New[allF(fid,0)];
        outF(i,1) = old2New[allF(fid,1)];
        outF(i,2) = old2New[allF(fid,2)];
    }    
}

class distItem{
public:
    bool is_nan;
    float dist;
#if FOR_DEBUG
    // Vector3d closestPnt;
#endif
public:
    distItem(): is_nan(), dist() {}
    distItem(bool nan, float distance): is_nan(nan), dist(distance) {}

};

void computeVert2FacesAndEdge2Faces(const MatrixXd& V, const MatrixXi& F,
                                        map<pair<int, int>, vector<int>>& edge2Face,
                                        map<int, vector<int>>& vert2Face)
{
    for (int i=0; i < F.rows(); ++i)
    {
        int v0 = F.row(i)[0], v1 = F.row(i)[1], v2 = F.row(i)[2];
        if (v0 < v1)
            edge2Face[CEdge(v0, v1)].push_back(i);
        else
            edge2Face[CEdge(v1, v0)].push_back(i);
        if (v0 < v2)
            edge2Face[CEdge(v0, v2)].push_back(i);
        else
            edge2Face[CEdge(v2, v0)].push_back(i);
        if (v2 < v1)
            edge2Face[CEdge(v2, v1)].push_back(i);
        else
            edge2Face[CEdge(v1, v2)].push_back(i);
        
        vert2Face[v0].push_back(i);
        vert2Face[v1].push_back(i);
        vert2Face[v2].push_back(i);            
    }
}

vector<double> computeT3PoleDistForPtsInCell(
    const MatrixXd& V,
    const MatrixXi& F,
    const vector<Vector3d>& pts,
    const pair<Vector3d, Vector3d>& cell,  // first - cell center, second - cell length
    const int cell_id
) {
  int faceNum = F.rows();
  Vector3d cc = cell.first;
  Vector3d length = cell.second;
  vector<int> includedFaces;  // store all faces that intersect with the current cell
  // collect intersected face ids
  // #pragma omp parallel for shared(count)
  for (int fid = 0; fid < faceNum; fid++) {
    float cellCenter[3] = {cc[0], cc[1], cc[2]};
    float halfSize[3] = {0.51 * length[0], 0.51 * length[1], 0.51 * length[2]};
    Vector3i f = F.row(fid);
    float triVerts[3][3] = {{V.row(f[0])[0], V.row(f[0])[1], V.row(f[0])[2]},
        {V.row(f[1])[0], V.row(f[1])[1], V.row(f[1])[2]},
        {V.row(f[2])[0], V.row(f[2])[1], V.row(f[2])[2]}};

    if (triBoxOverlap(cellCenter, halfSize, triVerts)) {
      includedFaces.push_back(fid);
    }
  }  // end for fid

  vector<double> gridDists; // output

  // // for debug only
  // if (cell_id == 855 || cell_id == 856 || cell_id == 859 || cell_id == 860) {
  //   std::cout << "included faces: " << std::endl;
  //   for (auto idx : includedFaces)
  //     std::cout << idx << std::endl;
  // }

  if (includedFaces.size() == 0) {
    for (int x = 0; x < pts.size(); x++) {
      gridDists.push_back(NAN);
    }
    return gridDists;
  }
  // std::cout << "There are " << includedFaces.size() << " faces inside!" << std::endl;


  // reorder the included trianlge faces
  MatrixXi partF;
  MatrixXd partV;
  reOrderMeshIndices(V, F, includedFaces, partV, partF);
  // igl::writeOBJ("../data/output.obj", partV, partF);

  // initialize PQP model
  PQP_Model* m_pqp_model = new PQP_Model();
  buildPQP(m_pqp_model, partV, partF); 


  map<pair<int, int>, vector<int>> part_edge2Face;
  map<int, vector<int>> part_vert2Face;
  computeVert2FacesAndEdge2Faces(partV, partF, part_edge2Face, part_vert2Face);

  MatrixXd face_normals; // per face normal
  // MatrixXd part_Z;
  // igl::writeOBJ("test.obj", partV, partF);
  igl::per_face_normals(partV, partF, Vector3d(1,1,1).normalized(), face_normals);

  // iterate over cell corners - compute its distance to the included faces
  // #pragma omp parallel for
  for (int k = 0; k < pts.size(); k++) {    
    const Vector3d& pnt = pts[k];
    // std::cout << pnt[0] << " " << pnt[1] << " " << pnt[2] << std::endl;

    Vector3d nearestPnt;
    int closestTriID;
    double dist = PQPABSDist(m_pqp_model, pnt, nearestPnt, closestTriID);
    RowVector3d n = face_normals.row(closestTriID);
    Vector3d dir_org = pnt - nearestPnt;
    Vector3d dir = dir_org.normalized();

    // double p2pdist = dir_org.norm();
    if (dist < 1e-10 || isnan(dir[0])) {
      dist = 0.0;
      gridDists.push_back(dist);
      continue;
    }
    double eps = 1e-4;
    double dotprod = dir.dot(n);
    if (abs(dotprod-1.0) < eps)
      dotprod = 1.0;
    if (abs(dotprod+1.0) < eps)
      dotprod = -1.0;
    double angle = acos(dotprod) * 180.0 / PI;

    if (abs(dotprod - 1.0) < eps) {
      // in this case, the nearest point lies inside certain triangle
      gridDists.push_back(dist);
      continue;
    } else if (abs(dotprod + 1.0) < eps) {
      gridDists.push_back(-dist);
      continue;
    } else {
      float a, b, c;  // barycentric coordinate
      int vid0 = partF.row(closestTriID)[0], vid1 = partF.row(closestTriID)[1], vid2 = partF.row(closestTriID)[2];
      Vector3d v0 = partV.row(vid0);
      Vector3d v1 = partV.row(vid1);
      Vector3d v2 = partV.row(vid2);
      Barycentric(nearestPnt, v0, v1, v2, a, b, c);
      float eps = 1e-4;

      if ( abs(a) < eps || abs(b) < eps || abs(c) < eps) {
        // closest point lies on the edge
        double prod = -10;
        if (abs(a-1.0) < eps ||  abs(b-1.0) < eps ||  abs(c-1.0) < eps) {
          int vid = vid0;
          if (abs(b-1.0) < eps)
              vid = vid1;
          if (abs(c-1.0) < eps)
              vid = vid2;
          prod = getMaxAngleViaVertConnection(vid, dir, face_normals, part_vert2Face);
          // prod = computeSignViaPseudonormal(vid, V, F, dir, face_normals, part_vert2Face);
        }else {
          prod = getMaxAngleViaEdgeConnection(a, b, c, closestTriID, dir, partF, face_normals, part_edge2Face);
        }
        if (abs(prod-1.0) < eps)
          prod = 1.0;
        if (abs(prod+1.0) < eps)
          prod = -1.0;
        double finalAngle = acos(prod) * 180.0 / PI;
        double newDist;
        if (finalAngle < 90.0) {
          newDist = dist;
        } else {
          newDist = -dist;
        }
        gridDists.push_back(newDist);
      } else {
        // do not lie on the edge and the angle is not smaller than 90 degree
        // std::cout << "sounds like impossible case just happened!" << std::endl;
        gridDists.push_back(dist);
      }
    }
  }

  delete m_pqp_model;
  return gridDists;
}
