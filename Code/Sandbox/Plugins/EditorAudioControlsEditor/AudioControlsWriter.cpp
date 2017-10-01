// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "AudioControlsWriter.h"

#include "AudioAssetsManager.h"

#include <IAudioSystemEditor.h>
#include <IAudioSystemItem.h>
#include <CryString/StringUtils.h>
#include <CrySystem/File/CryFile.h>
#include <CrySystem/ISystem.h>
#include <ISourceControl.h>
#include <IEditor.h>
#include <QtUtil.h>
#include <ConfigurationManager.h>

using namespace PathUtil;

namespace ACE
{
string const CAudioControlsWriter::s_controlsPath = AUDIO_SYSTEM_DATA_ROOT CRY_NATIVE_PATH_SEPSTR "ace" CRY_NATIVE_PATH_SEPSTR;
string const CAudioControlsWriter::s_levelsFolder = "levels" CRY_NATIVE_PATH_SEPSTR;
uint const CAudioControlsWriter::s_currentFileVersion = 2;

//////////////////////////////////////////////////////////////////////////
string TypeToTag(EItemType const eType)
{
	switch (eType)
	{
	case EItemType::Parameter:
		return "ATLRtpc";
	case EItemType::Trigger:
		return "ATLTrigger";
	case EItemType::Switch:
		return "ATLSwitch";
	case EItemType::State:
		return "ATLSwitchState";
	case EItemType::Preload:
		return "ATLPreloadRequest";
	case EItemType::Environment:
		return "ATLEnvironment";
	}
	return "";
}

//////////////////////////////////////////////////////////////////////////
CAudioControlsWriter::CAudioControlsWriter(CAudioAssetsManager* pAssetsManager, IAudioSystemEditor* pAudioSystemImpl, std::set<string>& previousLibraryPaths)
	: m_pAssetsManager(pAssetsManager)
	, m_pAudioSystemImpl(pAudioSystemImpl)
{
	if ((pAssetsManager != nullptr) && (pAudioSystemImpl != nullptr))
	{
		size_t const libCount = pAssetsManager->GetLibraryCount();

		for (size_t i = 0; i < libCount; ++i)
		{
			CAudioLibrary& library = *pAssetsManager->GetLibrary(i);
			WriteLibrary(library);
			library.SetModified(false);
		}

		// Delete libraries that don't exist anymore from disk
		std::set<string> librariesToDelete;
		std::set_difference(previousLibraryPaths.begin(), previousLibraryPaths.end(), m_foundLibraryPaths.begin(), m_foundLibraryPaths.end(),
		                    std::inserter(librariesToDelete, librariesToDelete.begin()));

		for (auto const& name : librariesToDelete)
		{
			string const fullFilePath = PathUtil::GetGameFolder() + CRY_NATIVE_PATH_SEPSTR + name;
			DeleteLibraryFile(fullFilePath);
		}

		previousLibraryPaths = m_foundLibraryPaths;
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::WriteLibrary(CAudioLibrary& library)
{
	if (library.IsModified())
	{
		LibraryStorage libraryXmlNodes;

		size_t const itemCount = library.ChildCount();

		for (size_t i = 0; i < itemCount; ++i)
		{
			WriteItem(library.GetChild(i), "", libraryXmlNodes);
		}

		// If empty, force it to write an empty library at the root
		if (libraryXmlNodes.empty())
		{
			libraryXmlNodes[Utils::GetGlobalScope()].isDirty = true;
		}

		for (auto const& libraryPair : libraryXmlNodes)
		{
			string libraryPath = s_controlsPath;
			Scope const scope = libraryPair.first;

			if (scope == Utils::GetGlobalScope())
			{
				// no scope, file at the root level
				libraryPath += library.GetName();
			}
			else
			{
				// with scope, inside level folder
				libraryPath += s_levelsFolder + m_pAssetsManager->GetScopeInfo(scope).name + CRY_NATIVE_PATH_SEPSTR + library.GetName();
			}

			m_foundLibraryPaths.insert(libraryPath.MakeLower() + ".xml");

			SLibraryScope const& libScope = libraryPair.second;

			if (libScope.isDirty)
			{
				XmlNodeRef pFileNode = GetISystem()->CreateXmlNode("ATLConfig");
				pFileNode->setAttr("atl_name", library.GetName());
				pFileNode->setAttr("atl_version", s_currentFileVersion);
				int const numTypes = static_cast<int>(EItemType::NumTypes);

				for (int i = 0; i < numTypes; ++i)
				{
					if (i != static_cast<int>(EItemType::State))   // switch_states are written inside the switches
					{
						XmlNodeRef node = libScope.GetXmlNode((EItemType)i);

						if ((node != nullptr) && (node->getChildCount() > 0))
						{
							pFileNode->addChild(node);
						}
					}
				}

				// Editor data
				XmlNodeRef pEditorData = pFileNode->createNode("EditorData");

				if (pEditorData != nullptr)
				{
					XmlNodeRef pFoldersNode = pEditorData->createNode("Folders");

					if (pFoldersNode != nullptr)
					{
						WriteEditorData(&library, pFoldersNode);
						pEditorData->addChild(pFoldersNode);
					}

					pFileNode->addChild(pEditorData);
				}

				string const fullFilePath = PathUtil::GetGameFolder() + CRY_NATIVE_PATH_SEPSTR + libraryPath + ".xml";

				DWORD const fileAttributes = GetFileAttributesA(fullFilePath.c_str());

				if (fileAttributes & FILE_ATTRIBUTE_READONLY)
				{
					// file is read-only
					SetFileAttributesA(fullFilePath.c_str(), FILE_ATTRIBUTE_NORMAL);
				}

				pFileNode->saveToFile(fullFilePath);
				CheckOutFile(fullFilePath);
			}
		}
	}
	else
	{
		std::unordered_set<Scope> scopes;

		size_t const numChildren = library.ChildCount();

		for (size_t i = 0; i < numChildren; ++i)
		{
			CAudioAsset* const pItem = library.GetChild(i);
			GetScopes(pItem, scopes);
		}

		for (auto const scope : scopes)
		{
			string libraryPath = s_controlsPath;

			if (scope == Utils::GetGlobalScope())
			{
				// no scope, file at the root level
				libraryPath += library.GetName();
			}
			else
			{
				// with scope, inside level folder
				libraryPath += s_levelsFolder + m_pAssetsManager->GetScopeInfo(scope).name + CRY_NATIVE_PATH_SEPSTR + library.GetName();
			}

			m_foundLibraryPaths.insert(libraryPath.MakeLower() + ".xml");
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::WriteItem(CAudioAsset* const pItem, const string& path, LibraryStorage& library)
{
	if (pItem != nullptr)
	{
		if (pItem->GetType() == EItemType::Folder)
		{
			size_t const itemCount = pItem->ChildCount();

			for (size_t i = 0; i < itemCount; ++i)
			{
				// Use forward slash only to ensure cross platform compatibility.
				string newPath = path.empty() ? "" : path + "/";
				newPath += pItem->GetName();
				WriteItem(pItem->GetChild(i), newPath, library);
			}
		}
		else
		{
			CAudioControl* pControl = static_cast<CAudioControl*>(pItem);

			if (pControl != nullptr)
			{
				SLibraryScope& scope = library[pControl->GetScope()];
				scope.isDirty = true;
				WriteControlToXML(scope.GetXmlNode(pControl->GetType()), pControl, path);
			}
		}

		pItem->SetModified(false);
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::GetScopes(CAudioAsset const* const pItem, std::unordered_set<Scope>& scopes)
{
	if (pItem->GetType() == EItemType::Folder)
	{
		size_t const numChildren = pItem->ChildCount();

		for (size_t i = 0; i < numChildren; ++i)
		{
			GetScopes(pItem->GetChild(i), scopes);
		}
	}
	else
	{
		CAudioControl const* const pControl = static_cast<CAudioControl const*>(pItem);

		if (pControl != nullptr)
		{
			scopes.insert(pControl->GetScope());
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::WriteControlToXML(XmlNodeRef const pNode, CAudioControl* pControl, const string& path)
{
	EItemType const type = pControl->GetType();
	XmlNodeRef const pChildNode = pNode->createNode(TypeToTag(type));
	pChildNode->setAttr("atl_name", pControl->GetName());

	if (!path.empty())
	{
		pChildNode->setAttr("path", path);
	}

	if (type == EItemType::Trigger)
	{
		float const radius = pControl->GetRadius();

		if (radius > 0.0f)
		{
			pChildNode->setAttr("atl_radius", radius);
			float const fadeOutDistance = pControl->GetOcclusionFadeOutDistance();

			if (fadeOutDistance > 0.0f)
			{
				pChildNode->setAttr("atl_occlusion_fadeout_distance", fadeOutDistance);
			}
		}

		if (!pControl->IsMatchRadiusToAttenuationEnabled())
		{
			pChildNode->setAttr("atl_match_radius_attenuation", "0");
		}

	}

	if (type == EItemType::Switch)
	{
		size_t const size = pControl->ChildCount();

		for (size_t i = 0; i < size; ++i)
		{
			CAudioAsset* pItem = pControl->GetChild(i);

			if ((pItem != nullptr) && (pItem->GetType() == EItemType::State))
			{
				WriteControlToXML(pChildNode, static_cast<CAudioControl*>(pItem), "");
			}
		}
	}
	else if (type == EItemType::Preload)
	{
		if (pControl->IsAutoLoad())
		{
			pChildNode->setAttr("atl_type", "AutoLoad");
		}

		std::vector<dll_string> const& platforms = GetIEditor()->GetConfigurationManager()->GetPlatformNames();
		size_t const count = platforms.size();

		for (size_t i = 0; i < count; ++i)
		{
			XmlNodeRef pGroupNode = pChildNode->createNode("ATLPlatform");
			pGroupNode->setAttr("atl_name", platforms[i].c_str());
			WriteConnectionsToXML(pGroupNode, pControl, i);

			if (pGroupNode->getChildCount() > 0)
			{
				pChildNode->addChild(pGroupNode);
			}
		}
	}
	else
	{
		WriteConnectionsToXML(pChildNode, pControl);
	}

	pNode->addChild(pChildNode);
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::WriteConnectionsToXML(XmlNodeRef const pNode, CAudioControl* const pControl, const int platformIndex)
{
	if ((pControl != nullptr) && (m_pAudioSystemImpl != nullptr))
	{
		XMLNodeList& otherNodes = pControl->GetRawXMLConnections(platformIndex);

		XMLNodeList::const_iterator end = std::remove_if(otherNodes.begin(), otherNodes.end(), [](const SRawConnectionData& node) { return node.isValid; });
		otherNodes.erase(end, otherNodes.end());

		for (auto const& node : otherNodes)
		{
			// Don't add identical nodes!
			bool shouldAddNode = true;
			XmlNodeRef const tempNode = pNode->findChild(node.xmlNode->getTag());

			if (tempNode)
			{
				int const numAttributes1 = tempNode->getNumAttributes();
				int const numAttributes2 = node.xmlNode->getNumAttributes();

				if (numAttributes1 == numAttributes2)
				{
					const char* key1 = nullptr, * val1 = nullptr, * key2 = nullptr, * val2 = nullptr;
					shouldAddNode = false;

					for (int i = 0; i < numAttributes1; ++i)
					{
						tempNode->getAttributeByIndex(i, &key1, &val1);
						node.xmlNode->getAttributeByIndex(i, &key2, &val2);

						if ((_stricmp(key1, key2) != 0) || (_stricmp(val1, val2) != 0))
						{
							shouldAddNode = true;
							break;
						}
					}
				}
			}

			if (shouldAddNode)
			{
				pNode->addChild(node.xmlNode);
			}
		}

		int const size = pControl->GetConnectionCount();

		for (int i = 0; i < size; ++i)
		{
			ConnectionPtr const pConnection = pControl->GetConnectionAt(i);

			if (pConnection != nullptr)
			{
				if ((pControl->GetType() != EItemType::Preload) || (pConnection->IsPlatformEnabled(platformIndex)))
				{
					XmlNodeRef const pChild = m_pAudioSystemImpl->CreateXMLNodeFromConnection(pConnection, pControl->GetType());

					if (pChild != nullptr)
					{
						pNode->addChild(pChild);
						pControl->AddRawXMLConnection(pChild, true, platformIndex);
					}
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::CheckOutFile(string const& filepath)
{
	ISourceControl* const pSourceControl = GetIEditor()->GetSourceControl();

	if (pSourceControl != nullptr)
	{
		uint32 const fileAttributes = pSourceControl->GetFileAttributes(filepath.c_str());

		if (fileAttributes & SCC_FILE_ATTRIBUTE_MANAGED)
		{
			pSourceControl->CheckOut(filepath);
		}
		else if ((fileAttributes == SCC_FILE_ATTRIBUTE_INVALID) || (fileAttributes & SCC_FILE_ATTRIBUTE_NORMAL))
		{
			pSourceControl->Add(filepath, "(ACE Changelist)", ADD_WITHOUT_SUBMIT | ADD_CHANGELIST);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::DeleteLibraryFile(string const& filepath)
{
	ISourceControl* const pSourceControl = GetIEditor()->GetSourceControl();

	if ((pSourceControl != nullptr) && (pSourceControl->GetFileAttributes(filepath.c_str()) & SCC_FILE_ATTRIBUTE_MANAGED))
	{
		// if source control is connected, let it handle the delete
		pSourceControl->Delete(filepath, "(ACE Changelist)", DELETE_WITHOUT_SUBMIT | ADD_CHANGELIST);
		DeleteFile(filepath.c_str());
	}
	else
	{
		DWORD const fileAttributes = GetFileAttributesA(filepath.c_str());

		if ((fileAttributes == INVALID_FILE_ATTRIBUTES) || !DeleteFile(filepath.c_str()))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[Audio Controls Editor] Failed to delete file %s", filepath);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CAudioControlsWriter::WriteEditorData(CAudioAsset const* const pLibrary, XmlNodeRef const pParentNode) const
{
	if ((pParentNode != nullptr) && (pLibrary != nullptr))
	{
		size_t const itemCount = pLibrary->ChildCount();

		for (size_t i = 0; i < itemCount; ++i)
		{
			CAudioAsset const* const pItem = pLibrary->GetChild(i);

			if (pItem->GetType() == EItemType::Folder)
			{
				XmlNodeRef const pFolderNode = pParentNode->createNode("Folder");

				if (pFolderNode != nullptr)
				{
					pFolderNode->setAttr("name", pItem->GetName());
					WriteEditorData(pItem, pFolderNode);
					pParentNode->addChild(pFolderNode);
				}
			}
		}
	}
}
} // namespace ACE
