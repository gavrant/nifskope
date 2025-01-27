#include "BSMesh.h"
#include "message.h"
#include "gl/controllers.h"
#include "gl/glscene.h"
#include "gl/renderer.h"
#include "io/material.h"
#include "io/nifstream.h"
#include "io/MeshFile.h"
#include "model/nifmodel.h"

#include <QDir>
#include <QBuffer>


BoneWeightsUNorm::BoneWeightsUNorm(QVector<QPair<quint16, quint16>> weights)
{
	weightsUNORM.resize(weights.size());
	for ( int i = 0; i < weights.size(); i++ ) {
		weightsUNORM[i] = BoneWeightUNORM16(weights[i].first, weights[i].second / 65535.0);
	}
}

BSMesh::BSMesh( Scene * _scene, NifFieldConst _block )
	: Shape( _scene, _block )
{
}

void BSMesh::transformShapes()
{
}

void BSMesh::drawShapes(NodeList* secondPass, bool presort)
{
	if ( !scene->hasOption(Scene::ShowMarkers) && name.startsWith("EditorMarker") )
		return;

	// Draw translucent meshes in second pass
	if ( secondPass && drawInSecondPass ) {
		secondPass->add(this);
		return;
	}

	auto nif = NifModel::fromIndex(iBlock);
	if ( lodLevel != scene->lodLevel ) {
		lodLevel = scene->lodLevel;
		updateData();
	}

	glPushMatrix();
	glMultMatrix(viewTrans());

	glEnable(GL_POLYGON_OFFSET_FILL);
	if ( drawInSecondPass )
		glPolygonOffset(0.5f, 1.0f);
	else
		glPolygonOffset(1.0f, 2.0f);


	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, transVerts.constData());

	if ( Node::SELECTING ) {
		if ( scene->isSelModeObject() ) {
			glSelectionBufferColor( nodeId );
		} else {
			glColor4f( 0, 0, 0, 1 );
		}
	}

	if ( !Node::SELECTING ) {
		glEnable(GL_FRAMEBUFFER_SRGB);
		shader = scene->renderer->setupProgram(this, shader);

	} else {
		glDisable(GL_FRAMEBUFFER_SRGB);
	}

	if ( !Node::SELECTING ) {
		if ( transNorms.count() ) {
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, 0, transNorms.constData());
		}

		if ( transColors.count() && scene->hasOption(Scene::DoVertexColors) ) {
			glEnableClientState(GL_COLOR_ARRAY);
			glColorPointer(4, GL_FLOAT, 0, transColors.constData());
		} else {
			glColor(Color3(1.0f, 1.0f, 1.0f));
		}
	}

	if ( sortedTriangles.count() )
		glDrawElements(GL_TRIANGLES, sortedTriangles.count() * 3, GL_UNSIGNED_SHORT, sortedTriangles.constData());
	
	if ( !Node::SELECTING )
		scene->renderer->stopProgram();

	// TODO: Show wireframe always for now, until material CDB reading
	if ( !Node::SELECTING ) {
		glDisable(GL_LIGHTING);
		glDisable(GL_COLOR_MATERIAL);
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_NORMALIZE);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_CULL_FACE);

		glDisable(GL_FRAMEBUFFER_SRGB);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-0.5f, -1.5f);
		glLineWidth(1.4f);
		glColor(Color4(0.5, 0.5, 0.5, 0.1));
		for ( const Triangle& tri : sortedTriangles ) {
			glBegin(GL_TRIANGLES);
			glVertex(transVerts.value(tri.v1()));
			glVertex(transVerts.value(tri.v2()));
			glVertex(transVerts.value(tri.v3()));
			glEnd();
		}
	}

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	glDisable(GL_POLYGON_OFFSET_FILL);

	glPopMatrix();
}

void BSMesh::drawSelection() const
{
	if ( scene->hasOption(Scene::ShowNodes) )
		Node::drawSelection();

	if ( isHidden() || !scene->isSelModeObject() )
		return;

	auto& idx = scene->currentIndex;
	auto& blk = scene->currentBlock;
	auto nif = NifModel::fromValidIndex(blk);
	glDisable(GL_LIGHTING);
	glDisable(GL_COLOR_MATERIAL);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_NORMALIZE);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);

	glDisable(GL_FRAMEBUFFER_SRGB);
	glPushMatrix();
	glMultMatrix(viewTrans());

	if ( blk == iBlock ) {

		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -2.0f);

		glPointSize(1.5f);
		glLineWidth(1.6f);
		glNormalColor();
		for ( const Triangle& tri : sortedTriangles ) {
			glBegin(GL_TRIANGLES);
			glVertex(transVerts.value(tri.v1()));
			glVertex(transVerts.value(tri.v2()));
			glVertex(transVerts.value(tri.v3()));
			glEnd();
		}

		glDisable(GL_POLYGON_OFFSET_FILL);

#ifndef QT_NO_DEBUG
		drawSphereSimple(boundSphere.center, boundSphere.radius, 72);
#endif
	}

	glPopMatrix();
}

BoundSphere BSMesh::bounds() const
{
	if ( needUpdateBounds ) {
		needUpdateBounds = false;
		if ( transVerts.count() ) {
			boundSphere = BoundSphere(transVerts);
		} else {
			boundSphere = dataBound;
		}
	}

	return worldTrans() * boundSphere;
}

QString BSMesh::textStats() const
{
	return QString();
}

void BSMesh::forMeshIndex(const NifModel* nif, std::function<void(const QString&, int)>& f)
{
	for ( int i = 0; i < 4; i++ ) {
		auto meshArray = iMeshes.child(i, 0);
		bool hasMesh = nif->get<bool>(meshArray.child(0, 0));
		auto mesh = meshArray.child(1, 0);
		if ( hasMesh ) {
			auto meshPath = nif->get<QString>(mesh, "Mesh Path");
			if ( !meshPath.startsWith("geometries", Qt::CaseInsensitive) ) {
				meshPath = "geometries\\" + meshPath;
			}
			if ( !meshPath.endsWith(".mesh") ) {
				meshPath += ".mesh";
			}
			f(meshPath, i);
		}
	}
}

int BSMesh::meshCount()
{
	return meshes.size();
}

void BSMesh::updateImpl(const NifModel* nif, const QModelIndex& index)
{
	qDebug() << "updateImpl";
	Shape::updateImpl(nif, index);

	iData = index;
	iMeshes = nif->getIndex(index, "Meshes");
	meshes.clear();
	std::function<void(const QString&, int)> createMeshFile = [&](const QString& meshPath, int lodLevel) {
		auto mesh = std::make_shared<MeshFile>(meshPath);
		if ( mesh->isValid() ) {
			meshes.append(mesh);
			if ( lodLevel > 0 || mesh->lods.size() > 0 )
				emit nif->lodSliderChanged(true);
		}
	};

	forMeshIndex(nif, createMeshFile);
}

void BSMesh::updateDataImpl()
{
	qDebug() << "updateData";
	gpuLODs.clear();
	boneNames.clear();
	boneTransforms.clear();

	if ( meshes.size() == 0 )
		return;

	bool hasMeshLODs = meshes[0]->lods.size() > 0;
	int lodCount = (hasMeshLODs) ? meshes[0]->lods.size() + 1 : meshes.size();

	if ( hasMeshLODs && meshes.size() > 1 ) {
		qWarning() << "Both static and skeletal mesh LODs exist";
	}

	lodLevel = std::min( scene->lodLevel, Scene::MAX_LOD_LEVEL_STARFIELD );

	auto meshIndex = (hasMeshLODs) ? 0 : lodLevel;
	if ( lodCount > lodLevel ) {
		auto& mesh = meshes[meshIndex];
		if ( lodLevel - 1 >= 0 && lodLevel - 1 < mesh->lods.size() ) {
			sortedTriangles = mesh->lods[lodLevel - 1];
		}
		else {
			sortedTriangles = mesh->triangles;
		}
		transVerts = mesh->positions;
		coords = mesh->coords;
		transColors = mesh->colors;
		hasVertexColors = !transColors.empty();
		transNorms = mesh->normals;
		transTangents = mesh->tangents;
		transBitangents = mesh->bitangents;
		weightsUNORM = mesh->weights;
		gpuLODs = mesh->lods;

		boundSphere = BoundSphere(transVerts);
		boundSphere.applyInv(viewTrans());
	}

	auto links = model->getChildLinks(model->getBlockNumber(iBlock));
	for ( const auto link : links ) {
		auto idx = model->getBlockIndex(link);
		if ( model->blockInherits(idx, "BSShaderProperty") ) {
			materialPath = model->get<QString>(idx, "Name");
		} else if ( model->blockInherits(idx, "NiIntegerExtraData") ) {
			materialID = model->get<int>(idx, "Integer Data");
		} else if ( model->blockInherits(idx, "BSSkin::Instance") ) {
			iSkin = idx;
			iSkinData = model->getBlockIndex(model->getLink(model->getIndex(idx, "Data")));
			skinID = model->getBlockNumber(iSkin);

			auto iBones = model->getLinkArray(iSkin, "Bones");
			for ( const auto b : iBones ) {
				if ( b == -1 )
					continue;
				auto iBone = model->getBlockIndex(b);
				boneNames.append(model->resolveString(iBone, "Name"));
			}

			auto numBones = model->get<int>(iSkinData, "Num Bones");
			boneTransforms.resize(numBones);
			auto iBoneList = model->getIndex(iSkinData, "Bone List");
			for ( int i = 0; i < numBones; i++ ) {
				auto iBone = iBoneList.child(i, 0);
				Transform trans;
				trans.rotation = model->get<Matrix>(iBone, "Rotation");
				trans.translation = model->get<Vector3>(iBone, "Translation");
				trans.scale = model->get<float>(iBone, "Scale");
				boneTransforms[i] = trans;
			}
		}
	}
	// Do after dependent blocks above
	for ( const auto link : links ) {
		auto idx = model->getBlockIndex(link);
		if ( model->blockInherits(idx, "SkinAttach") ) {
			boneNames = model->getArray<QString>(idx, "Bones");
			if ( std::all_of(boneNames.begin(), boneNames.end(), [](const QString& name) { return name.isEmpty(); }) ) {
				boneNames.clear();
				auto iBones = model->getLinkArray(model->getIndex(iSkin, "Bones"));
				for ( const auto& b : iBones ) {
					auto iBone = model->getBlockIndex(b);
					boneNames.append(model->resolveString(iBone, "Name"));
				}
			}
		}
	}
}
