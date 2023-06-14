#ifndef TRIMESH_HEAT_METHOD
#define TRIMESH_HEAT_METHOD

#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <wrap/io_trimesh/import.h>
#include <wrap/io_trimesh/export.h>

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseLU>

#include <vector>
#include <cmath>
#include <unordered_map>
#include <iostream>

class MyEdge;
class MyFace;
class MyVertex;

struct MyUsedTypes : public vcg::UsedTypes<
    vcg::Use<MyVertex>::AsVertexType, 
    vcg::Use<MyEdge>::AsEdgeType, 
    vcg::Use<MyFace>::AsFaceType
>{};

class MyVertex : public vcg::Vertex<
    MyUsedTypes,
    vcg::vertex::Coord3f,
    vcg::vertex::VFAdj,
    vcg::vertex::Color4b, //
    vcg::vertex::Qualityf,
    vcg::vertex::BitFlags // needed for PLY export
>{};
class MyEdge : public vcg::Edge<
    MyUsedTypes
>{};
class MyFace : public vcg::Face<
    MyUsedTypes,
    vcg::face::VFAdj,
    vcg::face::FFAdj,
    vcg::face::VertexRef,
    vcg::face::Normal3f,
    vcg::face::Qualityf,
    vcg::face::Color4b //
>{};
class MyMesh : public vcg::tri::TriMesh<
    std::vector<MyVertex>, 
    std::vector<MyFace>,
    std::vector<MyEdge>
>{};


// foward declarations
inline Eigen::Vector3d toEigen(const vcg::Point3f& p);
inline double cotan(const Eigen::Vector3d& v0, const Eigen::Vector3d& v1);
inline void buildMassMatrix(MyMesh &mesh, Eigen::SparseMatrix<double> &mass);
inline void buildCotanMatrix(MyMesh &mesh, Eigen::SparseMatrix<double> &cotanOperator);
inline double computeAverageEdgeLength(MyMesh &mesh);
inline Eigen::MatrixX3d computeVertexGradient(MyMesh &mesh, const Eigen::VectorXd &heat);
inline Eigen::VectorXd computeVertexDivergence(MyMesh &mesh, const Eigen::MatrixX3d &field);
inline Eigen::MatrixX3d normalizeVectorField(const Eigen::MatrixX3d &field);
inline Eigen::VectorXd computeHeatMethodGeodesic(MyMesh &mesh, Eigen::VectorXd &initialConditions, double m);
// debugging functions
inline Eigen::VectorXd computeHeatMethodGeodesicVerbose(MyMesh &mesh, const Eigen::VectorXd &init_cond, double m);
inline void printSparseMatrix(Eigen::SparseMatrix<double> &mat);
inline void printVectorXd(Eigen::VectorXd &vec);
inline void printVectorX3d(Eigen::MatrixX3d &vec);
inline void saveMeshWithVertexScalarField(MyMesh &mesh, const Eigen::VectorXd scalarField, const char *fname);
inline void saveMeshWithFaceVectorField(MyMesh &mesh, const Eigen::MatrixX3d vectorField, const char *fname);



inline Eigen::Vector3d toEigen(const vcg::Point3f& p)
{
    return Eigen::Vector3d(p.X(), p.Y(), p.Z());
};


inline double cotan(const Eigen::Vector3d& v0, const Eigen::Vector3d& v1)
{
    // cos(theta) / sin(theta)
    return v0.dot(v1) / v0.cross(v1).norm();
};


inline void buildMassMatrix(MyMesh &mesh, Eigen::SparseMatrix<double> &mass){
    // compute area of all faces
    for (MyMesh::FaceIterator fi = mesh.face.begin(); fi != mesh.face.end(); ++fi)
    {
        vcg::Point3f p0 = fi->V(0)->P();
        vcg::Point3f p1 = fi->V(1)->P();
        vcg::Point3f p2 = fi->V(2)->P();
        double e0 = toEigen(p1 - p0).norm();
        double e1 = toEigen(p2 - p0).norm();
        double e2 = toEigen(p2 - p1).norm();
        double s = (e0 + e1 + e2) / 2;
        double area = std::sqrt(s * (s - e0) * (s - e1) * (s - e2));
        // we store face areas in quality field to avoid a hash table
        // this will also be useful for the gradient computation
        fi->Q() = area;
    }
    // compute area of the dual cell for each vertex
    for (int i = 0; i < mesh.VN(); ++i){
        MyVertex *vp = &mesh.vert[i];

        std::vector<MyFace*> faces;
        std::vector<int> indices;
        vcg::face::VFStarVF<MyFace>(vp, faces, indices);
        
        double area = 0;
        for (int j = 0; j < faces.size(); ++j)
        {
            area += faces[j]->Q();
        }
        area /= 3;
        mass.coeffRef(i, i) = area;
    }
}


inline void buildCotanMatrix(MyMesh &mesh, Eigen::SparseMatrix<double> &cotanOperator){
    // initialize a hashtable from vertex pointers to ids
    std::unordered_map<MyVertex*, int> vertex_ids;
    for (int i = 0; i < mesh.VN(); ++i){
        vertex_ids[&mesh.vert[i]] = i;
    }

    // iterate over all vertices to fill cotan matrix
    for (int i = 0; i < mesh.VN(); ++i){
        MyVertex *vp = &mesh.vert[i];
        MyFace *fp = vp->VFp();
        vcg::face::Pos<MyFace> pos(fp, vp);
        vcg::face::Pos<MyFace> start(fp, vp);
        // iterate over all incident edges of vp
        do {
            // get the vertex opposite to vp
            pos.FlipV();
            MyVertex *vo = pos.V();
            // move to left vertex
            pos.FlipE();pos.FlipV();
            MyVertex *vl = pos.V();
            // move back then to right vertex
            pos.FlipV();pos.FlipE(); // back to vo
            pos.FlipF();pos.FlipE();pos.FlipV();
            MyVertex *vr = pos.V();
            pos.FlipV();pos.FlipE();pos.FlipF();pos.FlipV(); // back to vp

            // compute cotan of left edges and right edges
            Eigen::Vector3d elf = toEigen(vo->P() - vl->P()); // far left edge
            Eigen::Vector3d eln = toEigen(vp->P() - vl->P()); // near left edge
            Eigen::Vector3d erf = toEigen(vp->P() - vr->P()); // far right edge
            Eigen::Vector3d ern = toEigen(vo->P() - vr->P()); // near right edge

            double cotan_l = cotan(elf, eln);
            double cotan_r = cotan(ern, erf);

            // add to the matrix
            cotanOperator.coeffRef(vertex_ids[vp], vertex_ids[vo]) = (cotan_l + cotan_r)/2;

            // move to the next edge
            pos.FlipF();pos.FlipE();
        } while (pos != start);
    }

    // compute diagonal entries
    for (int i = 0; i < mesh.VN(); ++i){
        cotanOperator.coeffRef(i, i) = -cotanOperator.row(i).sum();
    }
    
}


inline double computeAverageEdgeLength(MyMesh &mesh){
    // compute total length of all edges
    double total_length = 0;
    for (MyMesh::FaceIterator fi = mesh.face.begin(); fi != mesh.face.end(); ++fi)
    {
        vcg::Point3f p0 = fi->V(0)->P();
        vcg::Point3f p1 = fi->V(1)->P();
        vcg::Point3f p2 = fi->V(2)->P();
        double e2 = toEigen(p1 - p0).norm();
        double e1 = toEigen(p2 - p0).norm();
        double e0 = toEigen(p2 - p1).norm();
        total_length += (e0 + e1 + e2) / 2;
    }
    return total_length / (3./2. * mesh.FN());
}


inline Eigen::MatrixX3d computeVertexGradient(MyMesh &mesh, const Eigen::VectorXd &heat){
    Eigen::MatrixX3d heatGradientField(mesh.FN(), 3);
    // initialize a hashtable from vertex pointers to ids
    std::unordered_map<MyVertex*, int> vertex_ids;
    for (int i = 0; i < mesh.VN(); ++i){
        vertex_ids[&mesh.vert[i]] = i;
    }
    // compute gradient of heat function at each vertex
    for (int i = 0; i < mesh.FN(); ++i){
        MyFace *fp = &mesh.face[i];

        vcg::Point3f p0 = fp->V(0)->P();
        vcg::Point3f p1 = fp->V(1)->P();
        vcg::Point3f p2 = fp->V(2)->P();

        // normal unit vector
        Eigen::Vector3d n = toEigen(fp->N());
        n /= n.norm();
        // face area
        double faceArea = fp->Q();
        // (ORDERING): edge unit vectors (assuming counter-clockwise ordering)
        // note if the ordering is clockwise, the gradient will point in the opposite direction
        Eigen::Vector3d e0 = toEigen(p2 - p1);
        e0 /= e0.norm();
        Eigen::Vector3d e1 = toEigen(p0 - p2);
        e1 /= e1.norm();
        Eigen::Vector3d e2 = toEigen(p1 - p0);
        e2 /= e2.norm();
        // gradient unit vectors
        Eigen::Vector3d g0 = n.cross(e0); //v0 grad
        Eigen::Vector3d g1 = n.cross(e1); //v1 grad
        Eigen::Vector3d g2 = n.cross(e2); //v2 grad

        // add vertex gradient contributions
        Eigen::Vector3d total_grad = (
            g0 * heat(vertex_ids[fp->V(0)]) + 
            g1 * heat(vertex_ids[fp->V(1)]) + 
            g2 * heat(vertex_ids[fp->V(2)])
        ) / (2 * faceArea);

        heatGradientField.row(i) = total_grad;
    }
    return heatGradientField;
}


inline Eigen::MatrixX3d normalizeVectorField(const Eigen::MatrixX3d &field){
    Eigen::MatrixX3d normalizedField(field.rows(), 3);
    normalizedField.setZero();
    // normalize vector field at each vertex
    for (int i = 0; i < field.rows(); ++i){
        Eigen::Vector3d v = field.row(i);
        normalizedField.row(i) = v / v.norm();
    }
    return normalizedField;
}


inline Eigen::VectorXd computeVertexDivergence(MyMesh &mesh, const Eigen::MatrixX3d &field){
    Eigen::VectorXd divergence(mesh.VN());
    divergence.setZero();
    // initialize a hashtable from face pointers to ids
    std::unordered_map<MyFace*, int> face_ids;
    for (int i = 0; i < mesh.FN(); ++i){
        face_ids[&mesh.face[i]] = i;
    }

    // compute divergence of vector field at each vertex
    for (int i = 0; i < mesh.VN(); ++i){
        MyVertex *vp = &mesh.vert[i];

        std::vector<MyFace*> faces;
        std::vector<int> indices;
        vcg::face::VFStarVF<MyFace>(vp, faces, indices);
        for (int j = 0; j < faces.size(); ++j)
        {
            MyFace *fp = faces[j];
            int index = indices[j];
            vcg::Point3f p0 = fp->V(0)->P();
            vcg::Point3f p1 = fp->V(1)->P();
            vcg::Point3f p2 = fp->V(2)->P();
            // (ORDERING) edge vectors
            Eigen::Vector3d el, er, eo; //left, right, opposite to vp
            if (index == 0){
                el = toEigen(p2 - p0); //e1
                er = toEigen(p1 - p0); //e2
                eo = toEigen(p1 - p2); //[+-] e0
            } else if (index == 1){
                el = toEigen(p0 - p1); //e2
                er = toEigen(p2 - p1); //e0
                eo = toEigen(p0 - p2); //[+-] e1
            } else if (index == 2){ 
                el = toEigen(p1 - p2); //e0
                er = toEigen(p0 - p2); //e1
                eo = toEigen(p0 - p1); //[+-] e2
            }
            // compute left and right cotangents
            double cotl = cotan(el, eo);
            double cotr = cotan(er, eo);
            // normalize edge vectors after cotangent computation
            el /= -el.norm(); // invert orientation of left edge
            er /= er.norm();
            // add divergence contribution of given face
            Eigen::Vector3d x = field.row(face_ids[fp]);
            divergence(i) += (cotl * er.dot(x) + cotr * el.dot(x)) / 2;
        }
    }
    return divergence;
}


inline Eigen::VectorXd computeHeatMethodGeodesic(MyMesh &mesh, const Eigen::VectorXd &init_cond, double m = 1){
    vcg::tri::UpdateTopology<MyMesh>::VertexFace(mesh);
    vcg::tri::UpdateTopology<MyMesh>::FaceFace(mesh);
    vcg::tri::UpdateNormal<MyMesh>::PerFaceNormalized(mesh);

    Eigen::SparseMatrix<double> mass(mesh.VN(), mesh.VN());
    buildMassMatrix(mesh, mass);

    Eigen::SparseMatrix<double> cotanOperator(mesh.VN(), mesh.VN());
    buildCotanMatrix(mesh, cotanOperator);

    double avg_edge_len = computeAverageEdgeLength(mesh);
    double timestep = m * avg_edge_len * avg_edge_len;
    Eigen::SparseMatrix<double> system1(mesh.VN(), mesh.VN());
    system1 = mass - timestep * cotanOperator;

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> cholesky1(system1);
    Eigen::VectorXd heatflow = cholesky1.solve(init_cond); // (VN)

    Eigen::MatrixX3d heatGradient = computeVertexGradient(mesh, heatflow); // (FN, 3)

    Eigen::MatrixX3d normalizedVectorField = normalizeVectorField(-heatGradient); // (FN, 3)
    
    Eigen::VectorXd divergence = computeVertexDivergence(mesh, normalizedVectorField); // (VN)

    Eigen::SparseMatrix<double> system2(mesh.VN(), mesh.VN());
    system2 = cotanOperator; //+ 1e-6 * Eigen::Matrix<double,-1,-1>::Identity(mesh.VN(), mesh.VN());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> cholesky2(system2);
    
    Eigen::VectorXd geodesicDistance = cholesky2.solve(divergence);

    return geodesicDistance;
}


// DEBUGGING FUNCTIONS

inline void printSparseMatrix(Eigen::SparseMatrix<double> &mat){
    for (int k=0; k < mat.outerSize(); ++k){
        for (Eigen::SparseMatrix<double>::InnerIterator it(mat,k); it; ++it){
            std::cout << "(" << it.row() << "," << it.col() << ") = " << it.value() << std::endl;
        }
    }
}

inline void printVectorXd(Eigen::VectorXd &vec){
    for (int i = 0; i < vec.size(); ++i){
        std::cout << vec[i] << std::endl;
    }
}

inline void printVectorX3d(Eigen::MatrixX3d &vec){
    for (int i = 0; i < vec.rows(); ++i){
        std::cout << vec.row(i) << std::endl;
    }
}

inline void saveMeshWithVertexScalarField(MyMesh &mesh, const Eigen::VectorXd scalarField, const char *fname){
    // save scalar field into vertex color
    double norm = scalarField.norm();
    for (int i = 0; i < mesh.VN(); ++i){
        char gray = (char) ((scalarField[i] / norm) * 128)+128;
        vcg::Color4b color(gray,gray,gray, 255);
        mesh.vert[i].C() = color;
    }
    vcg::tri::io::ExporterPLY<MyMesh>::Save(mesh, fname, vcg::tri::io::Mask::IOM_VERTCOLOR); 
}

inline void saveMeshWithFaceVectorField(MyMesh &mesh, const Eigen::MatrixX3d vectorField, const char *fname){
    // save vector field into face color
    for (int i = 0; i < mesh.FN(); ++i){
        Eigen::Vector3d vec = vectorField.row(i);
        vec /= vec.norm();
        char r = (char) (vec[0] * 128)+128;
        char g = (char) (vec[1] * 128)+128;
        char b = (char) (vec[2] * 128)+128;
        vcg::Color4b color(r,g,b, 255);

        mesh.face[i].C() = color;
    }
    vcg::tri::io::ExporterPLY<MyMesh>::Save(mesh, fname, vcg::tri::io::Mask::IOM_FACECOLOR); 
}



inline Eigen::VectorXd computeHeatMethodGeodesicVerbose(MyMesh &mesh, const Eigen::VectorXd &init_cond, double m = 1){
    vcg::tri::UpdateTopology<MyMesh>::VertexFace(mesh);
    vcg::tri::UpdateTopology<MyMesh>::FaceFace(mesh);
    vcg::tri::UpdateNormal<MyMesh>::PerFaceNormalized(mesh);

    std::cout << "Computing Mass..." << std::endl;
    Eigen::SparseMatrix<double> mass(mesh.VN(), mesh.VN());
    buildMassMatrix(mesh, mass);
    printSparseMatrix(mass);
    std::cout << "Computing Cotan..." << std::endl;
    Eigen::SparseMatrix<double> cotanOperator(mesh.VN(), mesh.VN());
    buildCotanMatrix(mesh, cotanOperator);
    printSparseMatrix(cotanOperator);

    std::cout << "Computing Edge Length..." << std::endl;
    double avg_edge_len = computeAverageEdgeLength(mesh);
    std::cout << "Average Edge: " << avg_edge_len << std::endl;
    double timestep = m * avg_edge_len * avg_edge_len;
    std::cout << "Timestep: " << timestep << std::endl;
    Eigen::SparseMatrix<double> system1(mesh.VN(), mesh.VN());
    system1 = mass - timestep * cotanOperator;
    printSparseMatrix(system1);

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;

    std::cout << "Cholesky Factorization 1..." << std::endl;
    solver.compute(system1);
    if(solver.info() != Eigen::Success) {
        std::cout << "ERROR: Cholesky Factorization 1 failed" << std::endl; 
    }
    Eigen::VectorXd heatflow = solver.solve(init_cond);
    if(solver.info() != Eigen::Success) {
        std::cout << "ERROR: Solving System 1 failed" << std::endl; 
    }
    
    printVectorXd(heatflow);
    saveMeshWithVertexScalarField(mesh, heatflow, "1_heatflow.ply");

    std::cout << "Computing Gradient..." << std::endl;
    Eigen::MatrixX3d heatGradient = computeVertexGradient(mesh, heatflow);
    printVectorX3d(heatGradient);
    saveMeshWithFaceVectorField(mesh, heatGradient, "2_heatGradient.ply");

    std::cout << "Normalizing Gradient..." << std::endl;
    Eigen::MatrixX3d normalizedVectorField = normalizeVectorField(-heatGradient);
    printVectorX3d(normalizedVectorField);
    saveMeshWithFaceVectorField(mesh, normalizedVectorField, "3_normalizedVectorField.ply");
    
    std::cout << "Computing Divergence..." << std::endl;
    Eigen::VectorXd divergence = computeVertexDivergence(mesh, normalizedVectorField);
    printVectorXd(divergence);
    saveMeshWithVertexScalarField(mesh, divergence, "4_divergence.ply");

    Eigen::SparseMatrix<double> system2(mesh.VN(), mesh.VN());
    system2 = cotanOperator; //+ 1e-6 * Eigen::Matrix<double,-1,-1>::Identity(mesh.VN(), mesh.VN());
    printSparseMatrix(system2);

    std::cout << "Cholesky Factorization 2..." << std::endl;
    solver.compute(system2);
    if(solver.info() != Eigen::Success) {
        std::cout << "ERROR: Cholesky Factorization 2 failed" << std::endl; 
    }
    Eigen::VectorXd geodesicDistance = solver.solve(divergence);
    if(solver.info() != Eigen::Success) {
        std::cout << "ERROR: Solving System 2 failed" << std::endl; 
    }
    printVectorXd(geodesicDistance);

    return geodesicDistance;
}


#endif