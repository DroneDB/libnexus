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

#include "libnxs.h"
#include <iostream>
#include <iomanip>

#include <QStringList>

#include <wrap/system/qgetopt.h>

#include "meshstream.h"
#include "kdtree.h"
#include "../nxsbuild/nexusbuilder.h"
#include "plyloader.h"
#include "objloader.h"
#include "tsploader.h"

using namespace std;

NXS_DLL NXSErr nexusBuild(const char *input, const char *output){

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
	bool center = false;


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

		cout << "Components: " << input;
		if(has_normals) cout << " normals";
		if(has_colors) cout << " colors";
		if(has_textures) cout << " textures";
		cout << "\n";

		quint32 components = 0;
		if(!point_cloud) components |= NexusBuilder::FACES;

		if((!no_normals && (!point_cloud || has_normals)) || normals) {
			components |= NexusBuilder::NORMALS;
			cout << "Normals enabled\n";
		}
		if((has_colors  && !no_colors ) || colors ) {
			components |= NexusBuilder::COLORS;
			cout << "Colors enabled\n";
		}
		if(has_textures && !no_texcoords) {
			components |= NexusBuilder::TEXTURES;
			cout << "Textures enabled\n";
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
			cerr << "Exiting" << endl;
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
		builder.save(QString(output));

	} catch(QString error) {
		cerr << "Fatal error: " << qPrintable(error) << endl;
		returncode = NXSERR_EXCEPTION;

	} catch(const char *error) {
		cerr << "Fatal error: " << error << endl;
		returncode = NXSERR_EXCEPTION;
	}

	if(tree)   delete tree;
	if(stream) delete stream;

	return returncode;
}
