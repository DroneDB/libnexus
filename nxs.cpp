/*
Nexus

Copyright(C) 2012 - Federico Ponchio
ISTI - Italian National Research Council - Visual Computing Lab

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License (http://www.gnu.org/licenses/gpl.txt)
for more details.
*/

#include "nxs.h"
#include <iostream>
#include <iomanip>

#include <QStringList>
#include <QtPlugin>

#include <wrap/system/qgetopt.h>

#include "meshstream.h"
#include "kdtree.h"
#include "../nxsbuild/nexusbuilder.h"
#include "../common/qtnexusfile.h"
#include "../common/traversal.h"
#include "../nxsedit/extractor.h"
#include "plyloader.h"
#include "objloader.h"
#include "tsploader.h"

using namespace std;
using namespace nx;

NXS_DLL NXSErr nexusBuild(const char *input, const char *output){
#ifdef INITIALIZE_STATIC_LIBJPEG
	Q_IMPORT_PLUGIN(QJpegPlugin);
#endif
	// we create a QCoreApplication just so that QT loads image IO plugins (for jpg and tiff loading)
	int node_size = 1<<15;
	float texel_weight =0.1; //relative weight of texels.

	int top_node_size = 4096;
	float vertex_quantization = 0.0f;   //optionally quantize vertices position.
	int tex_quality(95);                //default jpg texture quality
	int ram_buffer(2000);               //Mb of ram to use
	int n_threads = 4;
	float scaling(0.5);                 //simplification ratio
	int skiplevels = 0;
	QString mtl;
	QString translate;
	// bool center = false;


	bool point_cloud = false;
	bool normals = false;
	bool no_normals = false;
	bool colors = false;
	bool no_colors = false;
	bool no_texcoords = false;
	bool useOrigTex = false;
	bool create_pow_two_tex = false;
	bool deepzoom = false;

	//BTREE options
	float adaptive = 0.333f;

	//Check parameters are correct
	QStringList inputs;
	inputs.append(input);

	vcg::Point3d origin(0, 0, 0);
	
	Stream *stream = 0;
	KDTree *tree = 0;
	NXSErr returncode = NXSERR_NONE;
	try {
		quint64 max_memory = (1<<20)*(uint64_t)ram_buffer/4; //hack 4 is actually an estimate...

		string input = "mesh";
		stream = new StreamSoup("cache_stream");

		stream->setVertexQuantization(vertex_quantization);
		stream->setMaxMemory(max_memory);
		stream->origin = origin;

		vcg::Point3d &o = stream->origin;
		
		//TODO: actually the stream will store textures or normals or colors even if not needed
		stream->load(inputs, mtl);

		bool has_colors = stream->hasColors();
		bool has_normals = stream->hasNormals();
		bool has_textures = stream->hasTextures();

		//cout << "Components: " << input;
		//if(has_normals) cout << " normals";
		//if(has_colors) cout << " colors";
		//if(has_textures) cout << " textures";
		//cout << "\n";

		quint32 components = 0;
		if(!point_cloud) components |= NexusBuilder::FACES;

		if((!no_normals && (!point_cloud || has_normals)) || normals) {
			components |= NexusBuilder::NORMALS;
			//cout << "Normals enabled\n";
		}
		if((has_colors  && !no_colors ) || colors ) {
			components |= NexusBuilder::COLORS;
			//cout << "Colors enabled\n";
		}
		if(has_textures && !no_texcoords) {
			components |= NexusBuilder::TEXTURES;
			//cout << "Textures enabled\n";
		}

		//WORKAROUND to save loading textures not needed
		if(!(components & NexusBuilder::TEXTURES)) {
			stream->textures.clear();
		}

		NexusBuilder builder(components);
		builder.skipSimplifyLevels = skiplevels;
		builder.setMaxMemory(max_memory);
		builder.n_threads = n_threads;
		builder.setScaling(scaling);
		builder.useNodeTex = !useOrigTex;
		builder.createPowTwoTex = create_pow_two_tex;
		if(deepzoom)
			builder.header.signature.flags |= nx::Signature::Flags::DEEPZOOM;
		builder.tex_quality = tex_quality;
		bool success = builder.initAtlas(stream->textures);
		if(!success) {
			//cerr << "Exiting" << endl;
			return NXSERR_EXCEPTION;
		}

		tree = new KDTreeSoup("cache_tree", adaptive);
		tree->setMaxMemory((1<<20)*(uint64_t)ram_buffer/2);
		KDTreeSoup *treesoup = dynamic_cast<KDTreeSoup *>(tree);
		if(treesoup) {
			treesoup->setMaxWeight(node_size);
			treesoup->texelWeight = texel_weight;
			treesoup->setTrianglesPerBlock(node_size);
		}

		builder.create(tree, stream, top_node_size);
		QString qOutput(output);
		bool compress = qOutput.endsWith(".nxz");
		if (compress){
			// Generate temporary uncompressed file first
			qOutput = qOutput + ".tmp.nxs";
		}

		builder.save(qOutput);

		if (compress){

			float coord_step = 0.0f; //approxismate step for quantization
			int position_bits = 0;
			float error_q = 0.1;
			int luma_bits = 6;
			int chroma_bits = 6;
			int alpha_bits = 5;
			int norm_bits = 10;
			float tex_step = 0.25;

			double error(-1.0);
			double max_size(0.0);
			int max_triangles(0.0);
			int max_level(-1);
			QString projection("");
			QString matrix("");
			QString imatrix("");
			QString compresslib("corto");

			bool info = false;
			bool check = false;
			bool drop_level = false;
			QString recompute_error;

			inputs.clear();
			inputs.append(qOutput);

			NexusData nexus;
			nexus.file = new QTNexusFile();
			bool read_only = true;
			if(!recompute_error.isEmpty())
				read_only = false;

			if(!nexus.open(inputs[0].toLatin1())) {
				//cerr << "Fatal error: could not open file " << qPrintable(inputs[0]) << endl;
				return NXSERR_EXCEPTION;
			}

			QString qCompressedOutput(output);

			Extractor extractor(&nexus);

			// if(max_size != 0.0)
			// 	extractor.selectBySize(max_size*(1<<20));

			// if(error != -1)
			// 	extractor.selectByError(error);

			// if(max_triangles != 0)
			// 	extractor.selectByTriangles(max_triangles);

			// if(max_level >= 0)
			// 	extractor.selectByLevel(max_level);

			// if(drop_level)
			// 	extractor.dropLevel();

			// bool invert = false;
			// if(!imatrix.isEmpty()) {
			// 	matrix = imatrix;
			// 	invert = true;
			// }
			// if(!matrix.isEmpty()) {
			// 	QStringList sl = matrix.split(":");
			// 	if(sl.size() != 16) {
			// 		cerr << "Wrong matrix: found only " << sl.size() << " elements" << endl;
			// 		exit(-1);
			// 	}
			// 	vcg::Matrix44f m;
			// 	for(int i = 0; i < sl.size(); i++)
			// 		m.V()[i] = sl.at(i).toFloat();
			// 	//if(invert)
			// 	//    m = vcg::Invert(m);

			// 	extractor.setMatrix(m);
			// }

			Signature signature = nexus.header.signature;
			signature.flags &= ~(Signature::MECO | Signature::CORTO);
			if(compresslib == "meco")
				signature.flags |= Signature::MECO;
			else if(compresslib == "corto")
				signature.flags |= Signature::CORTO;
			else {
				//cerr << "Unknown compression method: " << qPrintable(compresslib) << endl;
				return NXSERR_EXCEPTION;
			}
			if(coord_step) {  //global precision, absolute value
				extractor.error_factor = 0.0; //ignore error factor.
				//do nothing
			} else if(position_bits) {
				vcg::Sphere3f &sphere = nexus.header.sphere;
				coord_step = sphere.Radius()/pow(2.0f, position_bits);
				extractor.error_factor = 0.0;

			} else if(error_q) {
				//take node 0:
				uint32_t sink = nexus.header.n_nodes -1;
				coord_step = error_q*nexus.nodes[0].error/2;
				for(unsigned int i = 0; i < sink; i++){
					Node &n = nexus.nodes[i];
					Patch &patch = nexus.patches[n.first_patch];
					if(patch.node != sink)
						continue;
					double e = error_q*n.error/2;
					if(e < coord_step && e > 0)
						coord_step = e; //we are looking at level1 error, need level0 estimate.
				}
				extractor.error_factor = error_q;
			}
			//cout << "Vertex quantization step: " << coord_step << endl;
			//cout << "Texture quantization step: " << tex_step << endl;
			extractor.coord_q =(int)log2(coord_step);
			extractor.norm_bits = norm_bits;
			extractor.color_bits[0] = luma_bits;
			extractor.color_bits[1] = chroma_bits;
			extractor.color_bits[2] = chroma_bits;
			extractor.color_bits[3] = alpha_bits;
			extractor.tex_step = tex_step; //was (int)log2(tex_step * pow(2, -12));, moved to per node value
			//cout << "Texture step: " << extractor.tex_step << endl;

			//cout << "Saving with flag: " << signature.flags;
			/*if (signature.flags & Signature::MECO) cout << " (compressed with MECO)";
			else if (signature.flags & Signature::CORTO) cout << " (compressed with CORTO)";
			else cout << " (not compressed)";
			cout << endl;*/

			extractor.save(qCompressedOutput, signature);

			//cout << "Saving to file " << qPrintable(output) << endl;
		}

		if (compress){
			// Remove old tmp file
			QFile::remove(qOutput);
		}
	} catch(QString error) {
		//cerr << "Fatal error: " << qPrintable(error) << endl;
		returncode = NXSERR_EXCEPTION;

	} catch(const char *error) {
		//cerr << "Fatal error: " << error << endl;
		returncode = NXSERR_EXCEPTION;
	}

	if(tree)   delete tree;
	if(stream) delete stream;

	return returncode;
}
