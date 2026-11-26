# PyrA Project, Git ap795/PyrA
# Copyright (c) 2026 Alexandre Papine
# SPDX-License-Identifier: MIT

param(
	[string]$BuildRoot = '',
	[ValidateSet('Release', 'Debug')]
	[string[]]$Configurations = @('Release', 'Debug'),
	[ValidateSet('Btf', 'OpenJPH', 'DCMTK', 'JXL')]
	[string[]]$Components = @('Btf', 'OpenJPH', 'DCMTK', 'JXL'),
	[string]$CMake = 'cmake.exe',
	[ValidateRange(1, 64)]
	[int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
if (-not $BuildRoot) {
	$BuildRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'out/Deps'
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
$sdk = Join-Path $BuildRoot 'windows'
$build = Join-Path $BuildRoot 'windows-build'
if (-not $env:PROCESSOR_ARCHITECTURE -and [Environment]::Is64BitOperatingSystem) {
	$env:PROCESSOR_ARCHITECTURE = 'AMD64'
}

function Invoke-CMake {
	& $CMake @args
	if ($LASTEXITCODE -ne 0) { throw "CMake failed with exit code $LASTEXITCODE" }
}

function Build-Sdk([string]$Source, [string]$Name, [string]$Prefix, [string[]]$Options, [switch]$InstallPerConfig) {
	$binary = Join-Path $build $Name
	$configure = @(
		'-S', $Source, '-B', $binary, '-G', 'Visual Studio 18 2026', '-A', 'x64', '-T', 'v145,host=x64',
		"-DCMAKE_CONFIGURATION_TYPES=$($Configurations -join ';')",
		"-DCMAKE_INSTALL_PREFIX=$Prefix", '-DCMAKE_INSTALL_LIBDIR=lib'
	)
	Invoke-CMake @configure @Options
	foreach ($config in $Configurations) {
		$buildArgs = @('--build', $binary, '--config', $config, '--parallel', $Jobs, '--', '/nr:false', '/p:TrackFileAccess=false')
		Invoke-CMake @buildArgs
		$install = if ($InstallPerConfig) { Join-Path $Prefix $config } else { $Prefix }
		Invoke-CMake --install $binary --config $config --prefix $install
	}
}

if ('Btf' -in $Components) {
	$source = Join-Path $PSScriptRoot 'windows'
	foreach ($runtime in @('md', 'mt')) {
		Build-Sdk $source "btf-$runtime" (Join-Path $sdk "btf/$runtime") @("-DPYRA_BTF_RUNTIME=$runtime")
	}
}
if ('OpenJPH' -in $Components) {
	$source = Join-Path $PSScriptRoot 'OpenJPH'
	$prefix = Join-Path $sdk 'openjph'
	Build-Sdk $source 'openjph' $prefix @(
		'-DBUILD_SHARED_LIBS=OFF', '-DOJPH_BUILD_EXECUTABLES=OFF',
		'-DOJPH_BUILD_TESTS=OFF', '-DOJPH_ENABLE_TIFF_SUPPORT=OFF'
	)
	$notices = New-Item -ItemType Directory -Path (Join-Path $prefix 'share/pyra-dependencies/openjph') -Force
	Copy-Item -LiteralPath @((Join-Path $source 'LICENSE'), (Join-Path $source 'SOURCE-NOTICES.txt')) -Destination $notices.FullName
}
if ('DCMTK' -in $Components) {
	Build-Sdk (Join-Path $PSScriptRoot 'dcmtk370') 'dcmtk' (Join-Path $sdk 'dcmtk') @(
		'-DDCMTK_MODULES=ofstd;oflog;dcmdata;dcmimgle;dcmimage;dcmsr;dcmiod;dcmfg;dcmseg',
		'-DBUILD_APPS=OFF', '-DBUILD_SHARED_LIBS=OFF', '-DDCMTK_BUILD_TESTS=OFF',
		'-DDCMTK_COMPILE_WIN32_MULTITHREADED_DLL=ON', '-DDCMTK_DEFAULT_DICT=builtin',
		'-DDCMTK_ENABLE_CHARSET_CONVERSION=OFF', '-DDCMTK_ENABLE_STL=ON',
		'-DDCMTK_WITH_DOXYGEN=OFF', '-DDCMTK_WITH_ICONV=OFF', '-DDCMTK_WITH_ICU=OFF',
		'-DDCMTK_WITH_OPENJPEG=OFF', '-DDCMTK_WITH_OPENSSL=OFF', '-DDCMTK_WITH_PNG=OFF',
		'-DDCMTK_WITH_SNDFILE=OFF', '-DDCMTK_WITH_TIFF=OFF', '-DDCMTK_WITH_XML=OFF',
		'-DDCMTK_WITH_ZLIB=OFF', '-DDCMTK_WITH_THREADS=ON'
	) -InstallPerConfig
}
if ('JXL' -in $Components) {
	$source = Join-Path $PSScriptRoot 'libjxl'
	$prefix = Join-Path $sdk 'jxl'
	Build-Sdk $source 'jxl' $prefix @('-C', (Join-Path $PSScriptRoot 'libjxl_options.cmake')) -InstallPerConfig
	$notices = Join-Path $prefix 'share/pyra-dependencies'
	foreach ($notice in @(
		@('libjxl', 'LICENSE'), @('libjxl', 'PATENTS'), @('libjxl', 'SOURCE-NOTICES.txt'),
		@('highway', 'third_party/highway/LICENSE'), @('highway', 'third_party/highway/LICENSE-BSD3'),
		@('brotli', 'third_party/brotli/LICENSE'), @('skcms', 'third_party/skcms/LICENSE'))) {
		$folder = New-Item -ItemType Directory -Path (Join-Path $notices $notice[0]) -Force
		Copy-Item -LiteralPath (Join-Path $source $notice[1]) -Destination $folder.FullName
	}
}
Write-Output "Bundled Windows SDK: $sdk"
