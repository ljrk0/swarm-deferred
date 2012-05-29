
#include "cbase.h"
#include "tier0/icommandline.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "materialsystem/imaterialvar.h"
#include "filesystem.h"
#include "deferred/deferred_shared_common.h"

#include "vgui_controls/messagebox.h"

static CDeferredManagerClient __g_defmanager;
CDeferredManagerClient *GetDeferredManager()
{
	return &__g_defmanager;
}

static IViewRender *g_pCurrentViewRender = NULL;

IViewRender *GetViewRenderInstance()
{
	AssertMsg( g_pCurrentViewRender != NULL, "viewrender creation failed!" );

	return g_pCurrentViewRender;
}

CDeferredManagerClient::CDeferredManagerClient() : BaseClass( "DeferredManagerClient" )
{
	m_bDefRenderingEnabled = false;

	Q_memset( m_pMat_Def, 0, sizeof(IMaterial*) * DEF_MAT_COUNT );
	Q_memset( m_pKV_Def, 0, sizeof(KeyValues*) * DEF_MAT_COUNT );
}

CDeferredManagerClient::~CDeferredManagerClient()
{
}

#ifdef DEFERRED_DEV
void CopyDev()
{
	FileFindHandle_t handle;
	char steamappsPath[MAX_PATH*4];
	const char *pszGameDir = engine->GetGameDirectory();

	Q_strcpy( steamappsPath, pszGameDir );
	Q_StripLastDir( steamappsPath, sizeof(steamappsPath) );
	Q_StripLastDir( steamappsPath, sizeof(steamappsPath) );

	const char *pszName = g_pFullFileSystem->FindFirst( VarArgs( "%sSourceMods\\SwarmDeferred\\shaders\\fxc\\*", steamappsPath ), &handle );

	while ( pszName != NULL )
	{
		if ( Q_strlen( pszName ) > 4 )
		{
			char filename[MAX_PATH];
			Q_FileBase( pszName, filename, sizeof( filename ) );

			char filepath_src[MAX_PATH];
			char filepath_dst[MAX_PATH];
			Q_snprintf( filepath_src, sizeof( filepath_src ), VarArgs( "%sSourceMods\\SwarmDeferred\\shaders\\fxc\\%s.vcs\0", steamappsPath ), filename );
			Q_snprintf( filepath_dst, sizeof( filepath_dst ), VarArgs( "%scommon\\alien swarm\\platform\\shaders\\fxc\\%s.vcs\0", steamappsPath ), filename );

			Msg( "%s --> %s\n", filepath_src, filepath_dst );
			engine->CopyFile( filepath_src, filepath_dst );
		}

		pszName = g_pFullFileSystem->FindNext( handle );
	}

	g_pFullFileSystem->FindClose( handle );
}
#endif

bool CDeferredManagerClient::Init()
{
#ifdef DEFERRED_DEV
	CopyDev();
#endif

	AssertMsg( g_pCurrentViewRender == NULL, "viewrender already allocated?!" );

	const bool bForceDeferred = CommandLine() && CommandLine()->FindParm("-forcedeferred") != 0;
	const bool bSM30 = g_pMaterialSystemHardwareConfig->GetDXSupportLevel() >= 95;

	if ( bSM30 || bForceDeferred )
	{
		bool bGotDefShaderDll = ConnectDeferredExt();

		if ( bGotDefShaderDll )
		{
			m_bDefRenderingEnabled = true;
			GetDeferredExt()->EnableDeferredLighting();

			g_pCurrentViewRender = new CDeferredViewRender();

			ConVarRef r_shadows( "r_shadows" );
			r_shadows.SetValue( "0" );

			InitDeferredRTs( true );

			materials->AddModeChangeCallBack( &DefRTsOnModeChanged );

			InitializeDeferredMaterials();
		}
	}

	if ( !m_bDefRenderingEnabled )
	{
		Assert( g_pCurrentViewRender == NULL );

		Warning( "Your hardware does not seem to support shader model 3.0. If you think that this is an error (hybrid GPUs), add -forcedeferred as start parameter.\n" );
		g_pCurrentViewRender = new CViewRender();
	}
	else
	{
#define VENDOR_NVIDIA 0x10DE
#define VENDOR_INTEL 0x8086
#define VENDOR_ATI 0x1002
#define VENDOR_AMD 0x1022

#ifndef SHADOWMAPPING_USE_COLOR
		MaterialAdapterInfo_t info;
		materials->GetDisplayAdapterInfo( materials->GetCurrentAdapter(), info );

		if ( info.m_VendorID == VENDOR_ATI ||
			info.m_VendorID == VENDOR_AMD )
		{
			vgui::MessageBox *pATIWarning = new vgui::MessageBox("UNSUPPORTED HARDWARE", VarArgs( "AMD/ATI IS NOT YET SUPPORTED IN HARDWARE FILTERING MODE\n"
				"(cdeferred_manager_client.cpp #%i).", __LINE__ ) );

			pATIWarning->InvalidateLayout();
			pATIWarning->DoModal();
		}
#endif
	}

	return true;
}

void CDeferredManagerClient::Shutdown()
{
	ShutdownDeferredMaterials();
	ShutdownDeferredExt();

	if ( IsDeferredRenderingEnabled() )
	{
		materials->RemoveModeChangeCallBack( &DefRTsOnModeChanged );
	}

	delete g_pCurrentViewRender;
	g_pCurrentViewRender = NULL;
	view = NULL;
}

void CDeferredManagerClient::LevelInitPreEntity()
{
	if ( m_bDefRenderingEnabled )
		DoShaderOverride();
}

ImageFormat CDeferredManagerClient::GetShadowDepthFormat()
{
	ImageFormat f = g_pMaterialSystemHardwareConfig->GetShadowDepthTextureFormat();

	// hack for hybrid stuff
	if ( f == IMAGE_FORMAT_UNKNOWN )
		f = IMAGE_FORMAT_D16_SHADOW;

	return f;
}

ImageFormat CDeferredManagerClient::GetNullFormat()
{
	return g_pMaterialSystemHardwareConfig->GetNullTextureFormat();
}

void CDeferredManagerClient::InitializeDeferredMaterials()
{
#if DEBUG
	m_pKV_Def[ DEF_MAT_WIREFRAME_DEBUG ] = new KeyValues( "wireframe" );
	if ( m_pKV_Def[ DEF_MAT_WIREFRAME_DEBUG ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_WIREFRAME_DEBUG ]->SetString( "$color", "[1 0.5 0.1]" );
		m_pMat_Def[ DEF_MAT_WIREFRAME_DEBUG ] = materials->CreateMaterial( "__lightworld_wireframe", m_pKV_Def[ DEF_MAT_WIREFRAME_DEBUG ] );
	}
#endif

	m_pKV_Def[ DEF_MAT_LIGHT_GLOBAL ] = new KeyValues( "LIGHTING_GLOBAL" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_GLOBAL ] != NULL )
		m_pMat_Def[ DEF_MAT_LIGHT_GLOBAL ] = materials->CreateMaterial( "__lightpass_global", m_pKV_Def[ DEF_MAT_LIGHT_GLOBAL ] );

	m_pKV_Def[ DEF_MAT_LIGHT_POINT_FULLSCREEN ] = new KeyValues( "LIGHTING_WORLD" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_POINT_FULLSCREEN ] != NULL )
		m_pMat_Def[ DEF_MAT_LIGHT_POINT_FULLSCREEN ] = materials->CreateMaterial( "__lightpass_point_fs", m_pKV_Def[ DEF_MAT_LIGHT_POINT_FULLSCREEN ] );

	m_pKV_Def[ DEF_MAT_LIGHT_POINT_WORLD ] = new KeyValues( "LIGHTING_WORLD" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_POINT_WORLD ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_POINT_WORLD ]->SetInt( "$WORLDPROJECTION", 1 );
		m_pMat_Def[ DEF_MAT_LIGHT_POINT_WORLD ] = materials->CreateMaterial( "__lightpass_point_w", m_pKV_Def[ DEF_MAT_LIGHT_POINT_WORLD ] );
	}

	m_pKV_Def[ DEF_MAT_LIGHT_SPOT_FULLSCREEN ] = new KeyValues( "LIGHTING_WORLD" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_SPOT_FULLSCREEN ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_SPOT_FULLSCREEN ]->SetInt( "$LIGHTTYPE", DEFLIGHTTYPE_SPOT );
		m_pMat_Def[ DEF_MAT_LIGHT_SPOT_FULLSCREEN ] = materials->CreateMaterial( "__lightpass_spot_fs", m_pKV_Def[ DEF_MAT_LIGHT_SPOT_FULLSCREEN ] );
	}

	m_pKV_Def[ DEF_MAT_LIGHT_SPOT_WORLD ] = new KeyValues( "LIGHTING_WORLD" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_SPOT_WORLD ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_SPOT_WORLD ]->SetInt( "$LIGHTTYPE", DEFLIGHTTYPE_SPOT );
		m_pKV_Def[ DEF_MAT_LIGHT_SPOT_WORLD ]->SetInt( "$WORLDPROJECTION", 1 );
		m_pMat_Def[ DEF_MAT_LIGHT_SPOT_WORLD ] = materials->CreateMaterial( "__lightpass_spot_w", m_pKV_Def[ DEF_MAT_LIGHT_SPOT_WORLD ] );
	}


	/*

	lighting volumes

	*/

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_FULLSCREEN ] = new KeyValues( "LIGHTING_VOLUME" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_FULLSCREEN ] != NULL )
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_POINT_FULLSCREEN ] = materials->CreateMaterial( "__lightpass_point_vfs", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_FULLSCREEN ] );

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_WORLD ] = new KeyValues( "LIGHTING_VOLUME" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_WORLD ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_WORLD ]->SetInt( "$WORLDPROJECTION", 1 );
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_POINT_WORLD ] = materials->CreateMaterial( "__lightpass_point_v", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_POINT_WORLD ] );
	}

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_FULLSCREEN ] = new KeyValues( "LIGHTING_VOLUME" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_FULLSCREEN ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_FULLSCREEN ]->SetInt( "$LIGHTTYPE", DEFLIGHTTYPE_SPOT );
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_FULLSCREEN ] = materials->CreateMaterial( "__lightpass_spot_v", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_FULLSCREEN ] );
	}

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ] = new KeyValues( "LIGHTING_VOLUME" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ]->SetInt( "$WORLDPROJECTION", 1 );
		m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ]->SetInt( "$LIGHTTYPE", DEFLIGHTTYPE_SPOT );
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ] = materials->CreateMaterial( "__lightpass_spot_v", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_SPOT_WORLD ] );
	}

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_PREPASS ] = new KeyValues( "VOLUME_PREPASS" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_PREPASS ] != NULL )
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_PREPASS ] = materials->CreateMaterial( "__volume_prepass", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_PREPASS ] );

	m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_BLEND ] = new KeyValues( "VOLUME_BLEND" );
	if ( m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_BLEND ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_BLEND ]->SetString( "$BASETEXTURE", GetDefRT_VolumetricsBuffer( 0 )->GetName() );
		m_pMat_Def[ DEF_MAT_LIGHT_VOLUME_BLEND ] = materials->CreateMaterial( "__volume_blend", m_pKV_Def[ DEF_MAT_LIGHT_VOLUME_BLEND ] );
	}


	/*

	blur

	*/

	m_pKV_Def[ DEF_MAT_BLUR_G6_X ] = new KeyValues( "GAUSSIAN_BLUR_6" );
	if ( m_pKV_Def[ DEF_MAT_BLUR_G6_X ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_BLUR_G6_X ]->SetString( "$BASETEXTURE", GetDefRT_VolumetricsBuffer( 0 )->GetName() );
		m_pMat_Def[ DEF_MAT_BLUR_G6_X ] = materials->CreateMaterial( "__blurpass_vbuf_x", m_pKV_Def[ DEF_MAT_BLUR_G6_X ] );
	}

	m_pKV_Def[ DEF_MAT_BLUR_G6_Y ] = new KeyValues( "GAUSSIAN_BLUR_6" );
	if ( m_pKV_Def[ DEF_MAT_BLUR_G6_Y ] != NULL )
	{
		m_pKV_Def[ DEF_MAT_BLUR_G6_Y ]->SetString( "$BASETEXTURE", GetDefRT_VolumetricsBuffer( 1 )->GetName() );
		m_pKV_Def[ DEF_MAT_BLUR_G6_Y ]->SetInt( "$ISVERTICAL", 1 );
		m_pMat_Def[ DEF_MAT_BLUR_G6_Y ] = materials->CreateMaterial( "__blurpass_vbuf_y", m_pKV_Def[ DEF_MAT_BLUR_G6_Y ] );
	}

#if DEBUG
	for ( int i = 0; i < DEF_MAT_COUNT; i++ )
	{
		Assert( m_pKV_Def[ i ] != NULL );
		Assert( m_pMat_Def[ i ] != NULL );
	}
#endif
}

void CDeferredManagerClient::ShutdownDeferredMaterials()
{
	// not deleted on purpose!!!!!
	for ( int i = 0; i < DEF_MAT_COUNT; i++ )
	{
		if ( m_pKV_Def[ i ] != NULL )
			m_pKV_Def[ i ]->Clear();
		m_pKV_Def[ i ] = NULL;
	}
}

static void ShaderReplace( const char *szShadername, IMaterial *pMat )
{
	const char *pszOldShadername = pMat->GetShaderName();
	const char *pszMatname = pMat->GetName();

	KeyValues *msg = new KeyValues( szShadername );

	int nParams = pMat->ShaderParamCount();
	IMaterialVar **pParams = pMat->GetShaderParams();

	char str[ 512 ];

	for ( int i = 0; i < nParams; ++i )
	{
		IMaterialVar *pVar = pParams[ i ];
		const char *pVarName = pVar->GetName();

		if (!stricmp("$flags", pVarName) || 
			!stricmp("$flags_defined", pVarName) || 
			!stricmp("$flags2", pVarName) || 
			!stricmp("$flags_defined2", pVarName) )
			continue;

		MaterialVarType_t vartype = pVar->GetType();
		switch ( vartype )
		{
		case MATERIAL_VAR_TYPE_FLOAT:
			msg->SetFloat( pVarName, pVar->GetFloatValue() );
			break;

		case MATERIAL_VAR_TYPE_INT:
			msg->SetInt( pVarName, pVar->GetIntValue() );
			break;

		case MATERIAL_VAR_TYPE_STRING:
			msg->SetString( pVarName, pVar->GetStringValue() );
			break;

		case MATERIAL_VAR_TYPE_FOURCC:
			//Assert( 0 ); // JDTODO
			break;

		case MATERIAL_VAR_TYPE_VECTOR:
			{
				const float *pVal = pVar->GetVecValue();
				int dim = pVar->VectorSize();
				switch ( dim )
				{
				case 1:
					V_snprintf( str, sizeof( str ), "[%f]", pVal[ 0 ] );
					break;
				case 2:
					V_snprintf( str, sizeof( str ), "[%f %f]", pVal[ 0 ], pVal[ 1 ] );
					break;
				case 3:
					V_snprintf( str, sizeof( str ), "[%f %f %f]", pVal[ 0 ], pVal[ 1 ], pVal[ 2 ] );
					break;
				case 4:
					V_snprintf( str, sizeof( str ), "[%f %f %f %f]", pVal[ 0 ], pVal[ 1 ], pVal[ 2 ], pVal[ 3 ] );
					break;
				default:
					Assert( 0 );
					*str = 0;
				}
				msg->SetString( pVarName, str );
			}
			break;

		case MATERIAL_VAR_TYPE_MATRIX:
			{
				const VMatrix &matrix = pVar->GetMatrixValue();
				const float *pVal = matrix.Base();
				V_snprintf( str, sizeof( str ),
					"[%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f]",
					pVal[ 0 ],  pVal[ 1 ],  pVal[ 2 ],  pVal[ 3 ],
					pVal[ 4 ],  pVal[ 5 ],  pVal[ 6 ],  pVal[ 7 ],
					pVal[ 8 ],  pVal[ 9 ],  pVal[ 10 ], pVal[ 11 ],
					pVal[ 12 ], pVal[ 13 ], pVal[ 14 ], pVal[ 15 ] );
				msg->SetString( pVarName, str );
			}
			break;

		case MATERIAL_VAR_TYPE_TEXTURE:
						msg->SetString( pVarName, pVar->GetTextureValue()->GetName() );
			break;

		case MATERIAL_VAR_TYPE_MATERIAL:
						msg->SetString( pVarName, pVar->GetMaterialValue()->GetName() );
			break;
		}
	}

	bool alphaBlending = pMat->IsTranslucent() || pMat->GetMaterialVarFlag( MATERIAL_VAR_TRANSLUCENT );
	bool translucentOverride = pMat->IsAlphaTested() || pMat->GetMaterialVarFlag( MATERIAL_VAR_ALPHATEST ) || alphaBlending;

	bool bDecal = pszOldShadername != NULL && Q_stristr( pszOldShadername,"decal" ) != NULL ||
		pszMatname != NULL && Q_stristr( pszMatname, "decal" ) != NULL ||
		pMat->GetMaterialVarFlag( MATERIAL_VAR_DECAL );

	if ( bDecal )
	{
		msg->SetInt( "$decal", 1 );

		if ( alphaBlending )
			msg->SetInt( "$translucent", 1 );
	}
	else if ( translucentOverride )
	{
		msg->SetInt( "$alphatest", 1 );
	}

	if ( pMat->IsTwoSided() )
	{
		msg->SetInt( "$nocull", 1 );
	}

	pMat->SetShaderAndParams(msg);

	pMat->RefreshPreservingMaterialVars();

	msg->deleteThis();
}

void CDeferredManagerClient::DoShaderOverride()
{
#ifndef DEFERRED_DEV
	for ( static int iBitchCount = 0; iBitchCount < 42; iBitchCount++ )
		Warning( ":::: YOU ARE PERFORMING THE RUNTIME SHADER OVERRIDE. DO NOT RELEASE THIS WAY. ::::\n" );
#endif
	bool bSanityCheck = false;

	for ( MaterialHandle_t hCurMat = materials->FirstMaterial();
		hCurMat != materials->InvalidMaterial();
		hCurMat = materials->NextMaterial( hCurMat ) )
	{
		IMaterial *pMat = materials->GetMaterial( hCurMat );

		if ( IsErrorMaterial( pMat ) )
			continue;

		pMat->FindVar( "$basetexture", &bSanityCheck, false );
		if ( !bSanityCheck )
			continue;

		const char *pszShadername = pMat->GetShaderName();

		if ( !pszShadername || !Q_strlen( pszShadername ) )
			continue;

		const char *pszReplace = NULL;

		if ( Q_stristr( pszShadername, "vertexlitgeneric" ) == pszShadername )
		{
			pszReplace = "DEFERRED_MODEL";
		}
		else if ( Q_stristr( pszShadername, "lightmappedgeneric" ) == pszShadername ||
			Q_stristr( pszShadername, "worldvertextransition" ) == pszShadername )
		{
			pszReplace = "DEFERRED_BRUSH";
		}

		if ( pszReplace != NULL )
			ShaderReplace( pszReplace, pMat );
	}

	materials->UncacheAllMaterials();
	materials->CacheUsedMaterials();
	materials->ReloadMaterials();
}

#ifdef DEFERRED_DEV
CON_COMMAND( deferred_DoShaderOverride, "" )
{
	GetDeferredManager()->DoShaderOverride();
}
#endif