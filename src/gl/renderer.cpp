/***** BEGIN LICENSE BLOCK *****

BSD License

Copyright (c) 2005-2015, NIF File Format Library and Tools
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the NIF File Format Library and Tools project may not be
   used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***** END LICENCE BLOCK *****/

#include "renderer.h"

#include "message.h"
#include "nifskope.h"
#include "gl/glshape.h"
#include "gl/glproperty.h"
#include "gl/glscene.h"
#include "gl/gltex.h"
#include "io/material.h"
#include "model/nifmodel.h"
#include "ui/settingsdialog.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QTextStream>


//! @file renderer.cpp Renderer and child classes implementation

bool shader_initialized = false;
bool shader_ready = true;

bool Renderer::initialize()
{
	if ( !shader_initialized ) {

		// check for OpenGL 2.0
		// (we don't use the extension API but the 2.0 API for shaders)
		if ( cfg.useShaders && fn->hasOpenGLFeature( QOpenGLFunctions::Shaders ) ) {
			shader_ready = true;
			shader_initialized = true;
		} else {
			shader_ready = false;
		}

		//qDebug() << "shader support" << shader_ready;
	}

	return shader_ready;
}

bool Renderer::hasShaderSupport()
{
	return shader_ready;
}

const QHash<Renderer::ConditionSingle::Type, QString> Renderer::ConditionSingle::compStrs{
	{ EQ,  " == " },
	{ NE,  " != " },
	{ LE,  " <= " },
	{ GE,  " >= " },
	{ LT,  " < " },
	{ GT,  " > " },
	{ AND, " & " },
	{ NAND, " !& " }
};

Renderer::ConditionSingle::ConditionSingle( const QString & line, bool neg ) : invert( neg )
{
	QHashIterator<Type, QString> i( compStrs );
	int pos = -1;

	while ( i.hasNext() ) {
		i.next();
		pos = line.indexOf( i.value() );

		if ( pos > 0 )
			break;
	}

	if ( pos > 0 ) {
		left  = line.left( pos ).trimmed();
		right = line.right( line.length() - pos - i.value().length() ).trimmed();

		if ( right.startsWith( "\"" ) && right.endsWith( "\"" ) )
			right = right.mid( 1, right.length() - 2 );

		comp = i.key();
	} else {
		left = line;
		comp = NONE;
	}
}

QModelIndex Renderer::ConditionSingle::getIndex( const NifModel * nif, const QVector<QModelIndex> & iBlocks, QString blkid ) const
{
	QString childid;

	if ( blkid.startsWith( "HEADER/" ) ) {
		auto blk = blkid.remove( "HEADER/" );
		if ( blk.contains("/") ) {
			auto blks = blk.split( "/" );
			return nif->getIndex( nif->getIndex( nif->getHeaderIndex(), blks.at(0) ), blks.at(1) );
		}
		return nif->getIndex( nif->getHeaderIndex(), blk );
	}

	int pos = blkid.indexOf( "/" );

	if ( pos > 0 ) {
		childid = blkid.right( blkid.length() - pos - 1 );
		blkid = blkid.left( pos );
	}

	for ( QModelIndex iBlock : iBlocks ) {
		if ( nif->blockInherits( iBlock, blkid ) ) {
			if ( childid.isEmpty() )
				return iBlock;

			return nif->getIndex( iBlock, childid );
		}
	}
	return QModelIndex();
}

bool Renderer::ConditionSingle::eval( const NifModel * nif, const QVector<QModelIndex> & iBlocks ) const
{
	QModelIndex iLeft = getIndex( nif, iBlocks, left );

	if ( !iLeft.isValid() )
		return invert;

	if ( comp == NONE )
		return !invert;

	const NifItem * item = nif->getItem( iLeft );
	if ( !item )
		return false;

	if ( item->isString() )
		return compare( item->getValueAsString(), right ) ^ invert;
	else if ( item->isCount() )
		return compare( item->getCountValue(), right.toULongLong( nullptr, 0 ) ) ^ invert;
	else if ( item->isFloat() )
		return compare( item->getFloatValue(), (float)right.toDouble() ) ^ invert;
	else if ( item->isFileVersion() )
		return compare( item->getFileVersionValue(), right.toUInt( nullptr, 0 ) ) ^ invert;
	else if ( item->valueType() == NifValue::tBSVertexDesc )
		return compare( (uint) item->get<BSVertexDesc>().GetFlags(), right.toUInt( nullptr, 0 ) ) ^ invert;

	return false;
}

bool Renderer::ConditionGroup::eval( const NifModel * nif, const QVector<QModelIndex> & iBlocks ) const
{
	if ( conditions.isEmpty() )
		return true;

	if ( isOrGroup() ) {
		for ( Condition * cond : conditions ) {
			if ( cond->eval( nif, iBlocks ) )
				return true;
		}
		return false;
	} else {
		for ( Condition * cond : conditions ) {
			if ( !cond->eval( nif, iBlocks ) )
				return false;
		}
		return true;
	}
}

void Renderer::ConditionGroup::addCondition( Condition * c )
{
	conditions.append( c );
}

Renderer::Shader::Shader( const QString & n, GLenum t, QOpenGLFunctions * fn )
	: f( fn ), name( n ), id( 0 ), status( false ), type( t )
{
	id = f->glCreateShader( type );
}

Renderer::Shader::~Shader()
{
	if ( id )
		f->glDeleteShader( id );
}

bool Renderer::Shader::load( const QString & filepath )
{
	try
	{
		QFile file( filepath );

		if ( !file.open( QIODevice::ReadOnly ) )
			throw QString( "couldn't open %1 for read access" ).arg( filepath );

		QByteArray data = file.readAll();

		const char * src = data.constData();

		f->glShaderSource( id, 1, &src, 0 );
		f->glCompileShader( id );

		GLint result;
		f->glGetShaderiv( id, GL_COMPILE_STATUS, &result );

		if ( result != GL_TRUE ) {
			GLint logLen;
			f->glGetShaderiv( id, GL_INFO_LOG_LENGTH, &logLen );
			char * log = new char[ logLen ];
			f->glGetShaderInfoLog( id, logLen, 0, log );
			QString errlog( log );
			delete[] log;
			throw errlog;
		}
	}
	catch ( QString & err )
	{
		status = false;
		Message::append( QObject::tr( "There were errors during shader compilation" ), QString( "%1:\r\n\r\n%2" ).arg( name ).arg( err ) );
		return false;
	}
	status = true;
	return true;
}


Renderer::Program::Program( const QString & n, QOpenGLFunctions * fn )
	: f( fn ), name( n ), id( 0 )
{
	id = f->glCreateProgram();
}

Renderer::Program::~Program()
{
	if ( id )
		f->glDeleteShader( id );
}

bool Renderer::Program::load( const QString & filepath, Renderer * renderer )
{
	try
	{
		QFile file( filepath );

		if ( !file.open( QIODevice::ReadOnly ) )
			throw QString( "couldn't open %1 for read access" ).arg( filepath );

		QTextStream stream( &file );

		QStack<ConditionGroup *> chkgrps;
		chkgrps.push( &conditions );

		while ( !stream.atEnd() ) {
			QString line = stream.readLine().trimmed();

			if ( line.startsWith( "shaders" ) ) {
				QStringList list = line.simplified().split( " " );

				for ( int i = 1; i < list.count(); i++ ) {
					Shader * shader = renderer->shaders.value( list[ i ] );

					if ( shader ) {
						if ( shader->status )
							f->glAttachShader( id, shader->id );
						else
							throw QString( "depends on shader %1 which was not compiled successful" ).arg( list[ i ] );
					} else {
						throw QString( "shader %1 not found" ).arg( list[ i ] );
					}
				}
			} else if ( line.startsWith( "checkgroup" ) ) {
				QStringList list = line.simplified().split( " " );

				if ( list.value( 1 ) == "begin" ) {
					ConditionGroup * group = new ConditionGroup( list.value( 2 ) == "or" );
					chkgrps.top()->addCondition( group );
					chkgrps.push( group );
				} else if ( list.value( 1 ) == "end" ) {
					if ( chkgrps.count() > 1 )
						chkgrps.pop();
					else
						throw QString( "mismatching checkgroup end tag" );
				} else {
					throw QString( "expected begin or end after checkgroup" );
				}
			} else if ( line.startsWith( "check" ) ) {
				line = line.remove( 0, 5 ).trimmed();

				bool invert = false;

				if ( line.startsWith( "not " ) ) {
					invert = true;
					line = line.remove( 0, 4 ).trimmed();
				}

				chkgrps.top()->addCondition( new ConditionSingle( line, invert ) );
			} else if ( line.startsWith( "texcoords" ) ) {
				line = line.remove( 0, 9 ).simplified();
				QStringList list = line.split( " " );
				bool ok;
				int unit = list.value( 0 ).toInt( &ok );
				QString idStr = list.value( 1 ).toLower();

				if ( !ok || idStr.isEmpty() )
					throw QString( "malformed texcoord tag" );

				int id = -1;
				if ( idStr == "tangents" )
					id = CT_TANGENT;
				else if ( idStr == "bitangents" )
					id = CT_BITANGENT;
				else if ( idStr == "indices" )
					id = CT_BONE;
				else if ( idStr == "weights" )
					id = CT_WEIGHT;
				else if ( idStr == "base" )
					id = TexturingProperty::getId( idStr );

				if ( id < 0 )
					throw QString( "texcoord tag refers to unknown texture id '%1'" ).arg( idStr );

				if ( texcoords.contains( unit ) )
					throw QString( "texture unit %1 is assigned twiced" ).arg( unit );

				texcoords.insert( unit, CoordType(id) );
			}
		}

		f->glLinkProgram( id );

		GLint result;

		f->glGetProgramiv( id, GL_LINK_STATUS, &result );

		if ( result != GL_TRUE ) {
			GLint logLen = 0;
			f->glGetProgramiv( id, GL_INFO_LOG_LENGTH, &logLen );

			if ( logLen != 0 ) {
				char * log = new char[ logLen ];
				f->glGetProgramInfoLog( id, logLen, 0, log );
				QString errlog( log );
				delete[] log;
				id = 0;
				throw errlog;
			}
		}
	}
	catch ( QString & x )
	{
		status = false;
		Message::append( QObject::tr( "There were errors during shader compilation" ), QString( "%1:\r\n\r\n%2" ).arg( name ).arg( x ) );
		return false;
	}
	status = true;
	return true;
}

void Renderer::Program::setUniformLocations()
{
	for ( int i = 0; i < NUM_UNIFORM_TYPES; i++ )
		uniformLocations[i] = f->glGetUniformLocation( id, uniforms[i].c_str() );
}

Renderer::Renderer( QOpenGLContext * c, QOpenGLFunctions * f )
	: cx( c ), fn( f )
{
	updateSettings();

	connect( NifSkope::getOptions(), &SettingsDialog::saveSettings, this, &Renderer::updateSettings );
}

Renderer::~Renderer()
{
	releaseShaders();
}


void Renderer::updateSettings()
{
	QSettings settings;

	cfg.useShaders = settings.value( "Settings/Render/General/Use Shaders", true ).toBool();

	bool prevStatus = shader_ready;

	shader_ready = cfg.useShaders && fn->hasOpenGLFeature( QOpenGLFunctions::Shaders );
	if ( !shader_initialized && shader_ready && !prevStatus ) {
		updateShaders();
		shader_initialized = true;
	}
}

void Renderer::updateShaders()
{
	if ( !shader_ready )
		return;

	releaseShaders();

	QDir dir( QCoreApplication::applicationDirPath() );

	if ( dir.exists( "shaders" ) )
		dir.cd( "shaders" );

#ifdef Q_OS_LINUX
	else if ( dir.exists( "/usr/share/nifskope/shaders" ) )
		dir.cd( "/usr/share/nifskope/shaders" );
#endif

	dir.setNameFilters( { "*.vert" } );
	for ( const QString& name : dir.entryList() ) {
		Shader * shader = new Shader( name, GL_VERTEX_SHADER, fn );
		shader->load( dir.filePath( name ) );
		shaders.insert( name, shader );
	}

	dir.setNameFilters( { "*.frag" } );
	for ( const QString& name : dir.entryList() ) {
		Shader * shader = new Shader( name, GL_FRAGMENT_SHADER, fn );
		shader->load( dir.filePath( name ) );
		shaders.insert( name, shader );
	}

	dir.setNameFilters( { "*.prog" } );
	for ( const QString& name : dir.entryList() ) {
		Program * program = new Program( name, fn );
		program->load( dir.filePath( name ), this );
		program->setUniformLocations();
		programs.insert( name, program );
	}
}

void Renderer::releaseShaders()
{
	if ( !shader_ready )
		return;

	qDeleteAll( programs );
	programs.clear();
	qDeleteAll( shaders );
	shaders.clear();
}

QString Renderer::setupProgram( Shape * mesh, const QString & hint )
{
	PropertyList props;
	mesh->activeProperties( props );

	auto nif = NifModel::fromValidIndex(mesh->index());
	if ( !shader_ready 
		 || hint.isNull()
		 || mesh->scene->hasOption(Scene::DisableShaders)
		 || mesh->scene->hasVisMode(Scene::VisSilhouette)
		 || !nif
		 || (nif->getBSVersion() == 0)
	) {
		setupFixedFunction( mesh, props );
		return {};
	}

	QVector<QModelIndex> iBlocks;
	iBlocks << mesh->index();
	iBlocks << mesh->iData;
	for ( Property * p : props.hash() ) {
		iBlocks.append( p->index() );
	}

	if ( !hint.isEmpty() ) {
		Program * program = programs.value( hint );
		if ( program && program->status && setupProgram( program, mesh, props, iBlocks, false ) )
			return program->name;
	}

	for ( Program * program : programs ) {
		if ( program->status && setupProgram( program, mesh, props, iBlocks ) )
			return program->name;
	}

	stopProgram();
	setupFixedFunction( mesh, props );
	return {};
}

void Renderer::stopProgram()
{
	if ( shader_ready ) {
		fn->glUseProgram( 0 );
	}

	resetTextureUnits();
}

void Renderer::Program::uni1f( UniformType var, float x )
{
	f->glUniform1f( uniformLocations[var], x );
}

void Renderer::Program::uni2f( UniformType var, float x, float y )
{
	f->glUniform2f( uniformLocations[var], x, y );
}

void Renderer::Program::uni3f( UniformType var, float x, float y, float z )
{
	f->glUniform3f( uniformLocations[var], x, y, z );
}

void Renderer::Program::uni4f( UniformType var, float x, float y, float z, float w )
{
	f->glUniform4f( uniformLocations[var], x, y, z, w );
}

void Renderer::Program::uni1i( UniformType var, int val )
{
	f->glUniform1i( uniformLocations[var], val );
}

void Renderer::Program::uni3m( UniformType var, const Matrix & val )
{
	if ( uniformLocations[var] >= 0 )
		f->glUniformMatrix3fv( uniformLocations[var], 1, 0, val.data() );
}

void Renderer::Program::uni4m( UniformType var, const Matrix4 & val )
{
	if ( uniformLocations[var] >= 0 )
		f->glUniformMatrix4fv( uniformLocations[var], 1, 0, val.data() );
}

void Renderer::Program::uniSampler( BSShaderProperty * bsprop, UniformType var,
									int textureSlot, int & texunit, const QString & alternate,
									TextureClampMode clamp, const QString & forced )
{
	GLint uniSamp = uniformLocations[var];
	if ( uniSamp >= 0 ) {

		// TODO: On stream 155 bsprop->fileName can reference incorrect strings because
		// the BSSTS is not filled out nor linked from the BSSP
		QString fname = forced.isEmpty() ? bsprop->fileName( textureSlot ) : forced;
		if ( fname.isEmpty() )
			fname = alternate;

		if ( !fname.isEmpty() && (!activateTextureUnit( texunit ) 
								   || !(bsprop->bind( textureSlot, fname, clamp ) 
										|| bsprop->bind( textureSlot, alternate, TextureClampMode::WrapS_WrapT ))) )
		{
			uniSamplerBlank( var, texunit );
		} else {
			f->glUniform1i( uniSamp, texunit++ );
		}
	}
}

void Renderer::Program::uniSamplerBlank( UniformType var, int & texunit )
{
	GLint uniSamp = uniformLocations[var];
	if ( uniSamp >= 0 && activateTextureUnit( texunit ) ) {
		glBindTexture( GL_TEXTURE_2D, 0 );
		f->glUniform1i( uniSamp, texunit++ );
	}
}

static QString white = "shaders/white.dds";
static QString black = "shaders/black.dds";
static QString lighting = "shaders/lighting.dds";
static QString gray = "shaders/gray.dds";
static QString magenta = "shaders/magenta.dds";
static QString default_n = "shaders/default_n.dds";
static QString default_ns = "shaders/default_ns.dds";
static QString cube = "shaders/cubemap.dds";

bool Renderer::setupProgram( Program * prog, Shape * mesh, const PropertyList & props,
							 const QVector<QModelIndex> & iBlocks, bool eval )
{
	auto nif = NifModel::fromValidIndex( mesh->index() );
	if ( !nif )
		return false;

	if ( eval && !prog->conditions.eval( nif, iBlocks ) )
		return false;

	fn->glUseProgram( prog->id );

	auto nifVersion = nif->getBSVersion();
	auto scene = mesh->scene;
	auto bsprop = mesh->bssp;
	auto lsp = mesh->bslsp;
	auto esp = mesh->bsesp;

	Material * mat = bsprop ? bsprop->getMaterial() : nullptr;

	QString default_n = (nifVersion >= 151) ? ::default_ns : ::default_n;

	// TODO: Temp for pre CDB material reading (Starfield)
	if ( !mat && nifVersion >= 172 ) {
		if ( lsp ) {
			mesh->depthWrite = true;
			mesh->depthTest = true;
		} else if ( esp ) {
			mesh->depthWrite = false;
			mesh->depthTest = false;
		}
	}

	// texturing

	TexturingProperty * texprop = props.get<TexturingProperty>();
	// BSShaderProperty * bsprop = props.get<BSShaderProperty>();
	// TODO: BSLSP has been split off from BSShaderProperty so it needs
	//	to be accessible from here

	TextureClampMode clamp = bsprop ? bsprop->clampMode : TextureClampMode::WrapS_WrapT;

	int texunit = 0;
	if ( bsprop ) {
		QString forced;
		if ( scene->hasOption(Scene::DoLighting) && scene->hasVisMode(Scene::VisNormalsOnly) )
			forced = white;

		QString alt = white;
		if ( scene->hasOption(Scene::DoErrorColor) && nifVersion < 172 ) // TODO: Hide error color until CDB reading
			alt = magenta;

		prog->uniSampler( bsprop, SAMP_BASE, 0, texunit, alt, clamp, forced );
	} else {
		GLint uniBaseMap = prog->uniformLocations[SAMP_BASE];
		if ( uniBaseMap >= 0 && (texprop || lsp) ) {
			if ( !activateTextureUnit( texunit ) || (texprop && !texprop->bind( 0 )) )
				prog->uniSamplerBlank( SAMP_BASE, texunit );
			else
				fn->glUniform1i( uniBaseMap, texunit++ );
		}
	}

	if ( bsprop && !esp ) {
		QString forced;
		if ( !scene->hasOption(Scene::DoLighting) )
			forced = default_n;
		prog->uniSampler( bsprop, SAMP_NORMAL, 1, texunit, default_n, clamp, forced );
	} else if ( !bsprop ) {
		GLint uniNormalMap = prog->uniformLocations[SAMP_NORMAL];
		if ( uniNormalMap >= 0 && texprop ) {
			bool result = true;
			if ( texprop ) {
				QString fname = texprop->fileName( 0 );
				if ( !fname.isEmpty() ) {
					int pos = fname.lastIndexOf( "_" );
					if ( pos >= 0 )
						fname = fname.left( pos ) + "_n.dds";
					else if ( (pos = fname.lastIndexOf( "." )) >= 0 )
						fname = fname.insert( pos, "_n" );
				}

				if ( !fname.isEmpty() && (!activateTextureUnit( texunit ) || !texprop->bind( 0, fname )) )
					result = false;
			}

			if ( !result )
				prog->uniSamplerBlank( SAMP_NORMAL, texunit );
			else
				fn->glUniform1i( uniNormalMap, texunit++ );
		}
	}

	if ( bsprop && !esp ) {
		prog->uniSampler( bsprop, SAMP_GLOW, 2, texunit, black, clamp );
	} else if ( !bsprop ) {
		GLint uniGlowMap = prog->uniformLocations[SAMP_GLOW];
		if ( uniGlowMap >= 0 && texprop ) {
			bool result = true;
			if ( texprop ) {
				QString fname = texprop->fileName( 0 );
				if ( !fname.isEmpty() ) {
					int pos = fname.lastIndexOf( "_" );
					if ( pos >= 0 )
						fname = fname.left( pos ) + "_g.dds";
					else if ( (pos = fname.lastIndexOf( "." )) >= 0 )
						fname = fname.insert( pos, "_g" );
				}

				if ( !fname.isEmpty() && (!activateTextureUnit( texunit ) || !texprop->bind( 0, fname )) )
					result = false;
			}

			if ( !result )
				prog->uniSamplerBlank( SAMP_GLOW, texunit );
			else
				fn->glUniform1i( uniGlowMap, texunit++ );
		}
	}

	// BSLightingShaderProperty
	if ( lsp ) {
		prog->uni1f( LIGHT_EFF1, lsp->lightingEffect1 );
		prog->uni1f( LIGHT_EFF2, lsp->lightingEffect2 );

		prog->uni1f( ALPHA, lsp->alpha );

		prog->uni2f( UV_SCALE, lsp->uvScale.x, lsp->uvScale.y );
		prog->uni2f( UV_OFFSET, lsp->uvOffset.x, lsp->uvOffset.y );

		prog->uni4m( MAT_VIEW, mesh->viewTrans().toMatrix4() );
		prog->uni4m( MAT_WORLD, mesh->worldTrans().toMatrix4() );

		prog->uni1i( G2P_COLOR, lsp->greyscaleColor );
		prog->uniSampler( bsprop, SAMP_GRAYSCALE, 3, texunit, "", TextureClampMode::MirrorS_MirrorT );

		prog->uni1i( HAS_TINT_COLOR, lsp->hasTintColor );
		if ( lsp->hasTintColor ) {
			prog->uni3f( TINT_COLOR, lsp->tintColor.red(), lsp->tintColor.green(), lsp->tintColor.blue() );
		}

		prog->uni1i( HAS_MAP_DETAIL, lsp->hasDetailMask );
		prog->uniSampler( bsprop, SAMP_DETAIL, 3, texunit, "shaders/blankdetailmap.dds", clamp );

		prog->uni1i( HAS_MAP_TINT, lsp->hasTintMask );
		prog->uniSampler( bsprop, SAMP_TINT, 6, texunit, gray, clamp );

		// Rim & Soft params

		prog->uni1i( HAS_SOFT, lsp->hasSoftlight );
		prog->uni1i( HAS_RIM, lsp->hasRimlight );

		prog->uniSampler( bsprop, SAMP_LIGHT, 2, texunit, default_n, clamp );

		// Backlight params

		prog->uni1i( HAS_MAP_BACK, lsp->hasBacklight );

		prog->uniSampler( bsprop, SAMP_BACKLIGHT, 7, texunit, default_n, clamp );

		// Glow params

		if ( scene->hasOption(Scene::DoGlow) && scene->hasOption(Scene::DoLighting) && (lsp->hasEmittance || nifVersion >= 151) )
			prog->uni1f( GLOW_MULT, lsp->emissiveMult );
		else
			prog->uni1f( GLOW_MULT, 0 );
		
		prog->uni1i( HAS_EMIT, lsp->hasEmittance );
		prog->uni1i( HAS_MAP_GLOW, lsp->hasGlowMap );
		prog->uni3f( GLOW_COLOR, lsp->emissiveColor.red(), lsp->emissiveColor.green(), lsp->emissiveColor.blue() );

		// Specular params
		float s = ( scene->hasOption(Scene::DoSpecular) && scene->hasOption(Scene::DoLighting) ) ? lsp->specularStrength : 0.0;
		prog->uni1f( SPEC_SCALE, s );

		// Assure specular power does not break the shaders
		prog->uni1f( SPEC_GLOSS, lsp->specularGloss);
		prog->uni3f( SPEC_COLOR, lsp->specularColor.red(), lsp->specularColor.green(), lsp->specularColor.blue() );
		prog->uni1i( HAS_MAP_SPEC, lsp->hasSpecularMap );

		if ( nifVersion <= 130 ) {
			if ( nifVersion == 130 || (lsp->hasSpecularMap && !lsp->hasBacklight) )
				prog->uniSampler( bsprop, SAMP_SPECULAR, 7, texunit, white, clamp );
			else
				prog->uniSampler( bsprop, SAMP_SPECULAR, 7, texunit, black, clamp );
		}

		if ( nifVersion >= 130 ) {
			prog->uni1i( DOUBLE_SIDE, lsp->isDoubleSided );
			prog->uni1f( G2P_SCALE, lsp->paletteScale );
			prog->uni1f( SS_ROLLOFF, lsp->lightingEffect1 );
			prog->uni1f( POW_FRESNEL, lsp->fresnelPower );
			prog->uni1f( POW_RIM, lsp->rimPower );
			prog->uni1f( POW_BACK, lsp->backlightPower );
		}

		// Multi-Layer

		prog->uniSampler( bsprop, SAMP_INNER, 6, texunit, default_n, clamp );
		if ( lsp->hasMultiLayerParallax ) {
			prog->uni2f( INNER_SCALE, lsp->innerTextureScale.x, lsp->innerTextureScale.y );
			prog->uni1f( INNER_THICK, lsp->innerThickness );

			prog->uni1f( OUTER_REFR, lsp->outerRefractionStrength );
			prog->uni1f( OUTER_REFL, lsp->outerReflectionStrength );
		}

		// Environment Mapping

		prog->uni1i( HAS_MAP_CUBE, lsp->hasEnvironmentMap );
		prog->uni1i( HAS_MASK_ENV, lsp->useEnvironmentMask );
		float refl = 0.0;
		if ( lsp->hasEnvironmentMap && scene->hasOption(Scene::DoCubeMapping) && scene->hasOption(Scene::DoLighting) )
			refl = lsp->environmentReflection;

		prog->uni1f( ENV_REFLECTION, refl );

		// Always bind cube regardless of shader settings
		GLint uniCubeMap = prog->uniformLocations[SAMP_CUBE];
		if ( uniCubeMap >= 0 ) {
			QString fname = bsprop->fileName( 4 );
			if ( fname.isEmpty() )
				fname = cube;

			if ( !activateTextureUnit( texunit ) )
				return false;
			if ( !bsprop->bindCube( fname ) && !bsprop->bindCube( cube ) )
				return false;

			fn->glUniform1i( uniCubeMap, texunit++ );
		}
		// Always bind mask regardless of shader settings
		prog->uniSampler( bsprop, SAMP_ENV_MASK, 5, texunit, white, clamp );

		if ( nifVersion >= 151 ) {
			prog->uniSampler( bsprop, SAMP_REFLECTIVITY, 8, texunit, black, clamp );
			prog->uniSampler( bsprop, SAMP_LIGHTING, 9, texunit, lighting, clamp );
		}

		// Parallax
		prog->uni1i( HAS_MAP_HEIGHT, lsp->hasHeightMap );
		prog->uniSampler( bsprop, SAMP_HEIGHT, 3, texunit, gray, clamp );
	}


	// BSEffectShaderProperty
	if ( esp ) {

		prog->uni4m( MAT_WORLD, mesh->worldTrans().toMatrix4() );

		prog->uniSampler( bsprop, SAMP_BASE, 0, texunit, white, clamp );

		prog->uni1i( DOUBLE_SIDE, esp->isDoubleSided );

		prog->uni2f( UV_SCALE, esp->uvScale.x, esp->uvScale.y );
		prog->uni2f( UV_OFFSET, esp->uvOffset.x, esp->uvOffset.y );

		prog->uni1i( HAS_MAP_BASE, esp->hasSourceTexture );
		prog->uni1i( HAS_MAP_G2P, esp->hasGreyscaleMap );

		prog->uni1i( G2P_ALPHA, esp->greyscaleAlpha );
		prog->uni1i( G2P_COLOR, esp->greyscaleColor );


		prog->uni1i( USE_FALLOFF, esp->useFalloff );
		prog->uni1i( HAS_RGBFALL, esp->hasRGBFalloff );
		prog->uni1i( HAS_WEAP_BLOOD, esp->hasWeaponBlood );

		// Glow params

		prog->uni4f( GLOW_COLOR, esp->emissiveColor.red(), esp->emissiveColor.green(), esp->emissiveColor.blue(), esp->emissiveColor.alpha() );
		prog->uni1f( GLOW_MULT, esp->emissiveMult );

		// Falloff params

		prog->uni4f( FALL_PARAMS,
			esp->falloff.startAngle, esp->falloff.stopAngle,
			esp->falloff.startOpacity, esp->falloff.stopOpacity
		);

		prog->uni1f( FALL_DEPTH, esp->falloff.softDepth );

		// BSEffectShader textures
		prog->uniSampler( bsprop, SAMP_GRAYSCALE, 1, texunit, "", TextureClampMode::MirrorS_MirrorT );

		if ( nifVersion >= 130 ) {

			prog->uni1f( LIGHT_INF, esp->lightingInfluence );

			prog->uni1i( HAS_MAP_NORMAL, esp->hasNormalMap && scene->hasOption(Scene::DoLighting) );

			prog->uniSampler( bsprop, SAMP_NORMAL, 3, texunit, default_n, clamp );

			prog->uni1i( HAS_MAP_CUBE, esp->hasEnvironmentMap );
			prog->uni1i( HAS_MASK_ENV, esp->hasEnvironmentMask );
			float refl = 0.0;
			if ( esp->hasEnvironmentMap && scene->hasOption(Scene::DoCubeMapping) && scene->hasOption(Scene::DoLighting) )
				refl = esp->environmentReflection;

			prog->uni1f( ENV_REFLECTION, refl );

			GLint uniCubeMap = prog->uniformLocations[SAMP_CUBE];
			if ( uniCubeMap >= 0 ) {
				QString fname = bsprop->fileName( 2 );
				if ( fname.isEmpty() )
					fname = cube;

				if ( !activateTextureUnit( texunit ) )
					return false;
				if ( !bsprop->bindCube( fname ) && !bsprop->bindCube( cube ) )
					return false;

				fn->glUniform1i( uniCubeMap, texunit++ );
			}
			prog->uniSampler( bsprop, SAMP_SPECULAR, 4, texunit, white, clamp );
			if ( nifVersion >= 151 ) {
				prog->uniSampler( bsprop, SAMP_REFLECTIVITY, 6, texunit, black, clamp );
				prog->uniSampler( bsprop, SAMP_LIGHTING, 7, texunit, lighting, clamp );
			}

			prog->uni1f( LUM_EMIT, esp->lumEmittance );
		}
	}

	// Defaults for uniforms in older meshes
	if ( !esp && !lsp ) {
		prog->uni2f( UV_SCALE, 1.0, 1.0 );
		prog->uni2f( UV_OFFSET, 0.0, 0.0 );
	}

	QMapIterator<int, Program::CoordType> itx( prog->texcoords );

	while ( itx.hasNext() ) {
		itx.next();

		if ( !activateTextureUnit( itx.key() ) )
			return false;

		auto it = itx.value();
		if ( it == Program::CT_TANGENT ) {
			if ( mesh->transTangents.count() ) {
				glEnableClientState( GL_TEXTURE_COORD_ARRAY );
				glTexCoordPointer( 3, GL_FLOAT, 0, mesh->transTangents.constData() );
			} else if ( mesh->tangents.count() ) {
				glEnableClientState( GL_TEXTURE_COORD_ARRAY );
				glTexCoordPointer( 3, GL_FLOAT, 0, mesh->tangents.constData() );
			} else {
				return false;
			}

		} else if ( it == Program::CT_BITANGENT ) {
			if ( mesh->transBitangents.count() ) {
				glEnableClientState( GL_TEXTURE_COORD_ARRAY );
				glTexCoordPointer( 3, GL_FLOAT, 0, mesh->transBitangents.constData() );
			} else if ( mesh->bitangents.count() ) {
				glEnableClientState( GL_TEXTURE_COORD_ARRAY );
				glTexCoordPointer( 3, GL_FLOAT, 0, mesh->bitangents.constData() );
			} else {
				return false;
			}
		} else if ( texprop ) {
			int txid = it;
			if ( txid < 0 )
				return false;

			int set = texprop->coordSet( txid );

			if ( set < 0 || !(set < mesh->coords.count()) || !mesh->coords[set].count() )
				return false;

			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glTexCoordPointer( 2, GL_FLOAT, 0, mesh->coords[set].constData() );
		} else if ( bsprop ) {
			int txid = it;
			if ( txid < 0 )
				return false;

			int set = 0;

			if ( set < 0 || !(set < mesh->coords.count()) || !mesh->coords[set].count() )
				return false;

			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glTexCoordPointer( 2, GL_FLOAT, 0, mesh->coords[set].constData() );
		}
	}

	// setup lighting

	//glEnable( GL_LIGHTING );

	// setup blending

	glProperty( mesh->alphaProperty );
	
	if ( mat && scene->hasOption(Scene::DoBlending) ) {
		static const GLenum blendMap[11] = {
			GL_ONE, GL_ZERO, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
			GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
			GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE
		};

		if ( mat->hasAlphaBlend() ) {
			glEnable( GL_BLEND );
			glBlendFunc( blendMap[mat->iAlphaSrc], blendMap[mat->iAlphaDst] );
		} else {
			glDisable( GL_BLEND );
		}

		if ( mat->hasAlphaTest() ) {
			glEnable( GL_ALPHA_TEST );
			glAlphaFunc( GL_GREATER, float( mat->iAlphaTestRef ) / 255.0 );
		} else {
			glDisable( GL_ALPHA_TEST );
		}
	}

	// BSESP/BSLSP do not always need an NiAlphaProperty, and appear to override it at times
	if ( !mat && mesh->translucent ) {
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		// If mesh is alpha tested, override threshold
		glAlphaFunc( GL_GREATER, 0.1f );
	}

	glDisable( GL_COLOR_MATERIAL );

	if ( nifVersion < 83 ) {
		// setup vertex colors

		//glProperty( props.get< VertexColorProperty >(), glIsEnabled( GL_COLOR_ARRAY ) );
		
		// setup material

		glProperty( props.get<MaterialProperty>(), props.get<SpecularProperty>() );

		// setup z buffer

		glProperty( props.get<ZBufferProperty>() );

		// setup stencil

		glProperty( props.get<StencilProperty>() );

		// wireframe ?

		glProperty( props.get<WireframeProperty>() );
	} else {
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_TRUE );
		glDepthFunc( GL_LEQUAL );
		glEnable( GL_CULL_FACE );
		glCullFace( GL_BACK );
		glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
	}

	if ( !mesh->depthTest ) {
		glDisable( GL_DEPTH_TEST );
	}

	if ( !mesh->depthWrite || mesh->translucent ) {
		glDepthMask( GL_FALSE );
	}

	return true;
}

void Renderer::setupFixedFunction( Shape * mesh, const PropertyList & props )
{
	// setup lighting

	glEnable( GL_LIGHTING );

	// Disable specular because it washes out vertex colors
	//	at perpendicular viewing angles
	float color[4] = { 0, 0, 0, 0 };
	glMaterialfv( GL_FRONT_AND_BACK, GL_SPECULAR, color );
	glLightfv( GL_LIGHT0, GL_SPECULAR, color );

	// setup blending

	glProperty( mesh->alphaProperty );

	// setup vertex colors

	glProperty( props.get<VertexColorProperty>(), glIsEnabled( GL_COLOR_ARRAY ) );

	// setup material

	glProperty( props.get<MaterialProperty>(), props.get<SpecularProperty>() );

	// setup texturing

	//glProperty( props.get< TexturingProperty >() );

	// setup z buffer

	glProperty( props.get<ZBufferProperty>() );

	if ( !mesh->depthTest ) {
		glDisable( GL_DEPTH_TEST );
	}

	if ( !mesh->depthWrite ) {
		glDepthMask( GL_FALSE );
	}

	// setup stencil

	glProperty( props.get<StencilProperty>() );

	// wireframe ?

	glProperty( props.get<WireframeProperty>() );

	// normalize

	if ( glIsEnabled( GL_NORMAL_ARRAY ) )
		glEnable( GL_NORMALIZE );
	else
		glDisable( GL_NORMALIZE );

	// setup texturing

	if ( !mesh->scene->hasOption(Scene::DoTexturing) )
		return;

	if ( TexturingProperty * texprop = props.get<TexturingProperty>() ) {
		// standard multi texturing property
		int stage = 0;

		if ( texprop->bind( 1, mesh->coords, stage ) ) {
			// dark
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 0, mesh->coords, stage ) ) {
			// base
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 2, mesh->coords, stage ) ) {
			// detail
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0 );
		}

		if ( texprop->bind( 6, mesh->coords, stage ) ) {
			// decal 0
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 7, mesh->coords, stage ) ) {
			// decal 1
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 8, mesh->coords, stage ) ) {
			// decal 2
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 9, mesh->coords, stage ) ) {
			// decal 3
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}

		if ( texprop->bind( 4, mesh->coords, stage ) ) {
			// glow
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );
		}
	} else if ( TextureProperty * texprop = props.get<TextureProperty>() ) {
		// old single texture property
		texprop->bind( mesh->coords );
	} else if ( BSShaderProperty * texprop = props.get<BSShaderProperty>() ) {
		// standard multi texturing property
		int stage = 0;

		if ( texprop->bind( 0, mesh->coords ) ) {
			//, mesh->coords, stage ) )
			// base
			stage++;
			glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR );

			glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA );
			glTexEnvi( GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE );
			glTexEnvi( GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA );

			glTexEnvf( GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0 );

			if ( mesh->translucent ) {
				glEnable( GL_BLEND );
				glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				// If mesh is alpha tested, override threshold
				glAlphaFunc( GL_GREATER, 0.1f );
			}
		}
	} else {
		glDisable( GL_TEXTURE_2D );
	}
}

