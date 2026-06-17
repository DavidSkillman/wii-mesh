#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <string>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#pragma pack(push, 1)
struct Header
{
	uint32_t materialIndex;
	uint16_t vertexCount;
	uint16_t indexCount;
};

struct VertexSkin
{
	uint8_t boneIDs[4];
	float weights[4];
};

struct Vertex
{
	float x, y, z;
	float nx, ny, nz;
	VertexSkin skin;
};

#pragma pack(pop)

struct Mesh
{
	uint32_t materialIndex;
	std::vector<Vertex> vertices;
	std::vector<uint16_t> indices;
};

std::unordered_map<std::string, uint8_t> boneMap;
std::unordered_map<std::string, std::string> boneParentMap;
std::vector<aiMatrix4x4> boneOffsets;

const uint32_t MAX_INDICES = 65535;
const uint32_t MAX_VERTS = 65535;

void BuildBoneParents(aiNode* node, const std::string& parent)
{
	std::string name = node->mName.C_Str();

	if (!parent.empty())
		boneParentMap[name] = parent;

	for (unsigned int i = 0; i < node->mNumChildren; i++)
	{
		BuildBoneParents(node->mChildren[i], name);
	}
}

int main(int argc, char *argv[])
{
	if (argc < 2) return 1;
	Assimp::Importer importer;

	const aiScene* scene = importer.ReadFile(
		argv[1],
		aiProcess_Triangulate |
		aiProcess_JoinIdenticalVertices |
		aiProcess_GenSmoothNormals
	);

	if (!scene || !scene->mRootNode)
	{
		std::cout << "Failed to load model" << std::endl;
		return -1;
	}

	std::cout << "Loaded successfully!" << std::endl;

	BuildBoneParents(scene->mRootNode, "");

	std::vector<Mesh> meshes;
	for (size_t m = 0; m < scene->mNumMeshes; m++)
	{
		aiMesh* mesh = scene->mMeshes[m];
		meshes.push_back(Mesh { mesh->mMaterialIndex });
		
		// vertices
		for (uint16_t i = 0; i < mesh->mNumVertices; i++)
		{
			Vertex v;

			v.x = mesh->mVertices[i].x;
			v.y = mesh->mVertices[i].y;
			v.z = mesh->mVertices[i].z;

			v.nx = mesh->mNormals[i].x;
			v.ny = mesh->mNormals[i].y;
			v.nz = mesh->mNormals[i].z;

			for (int i = 0; i < 4; i++)
			{
				v.skin.boneIDs[i] = 0;
				v.skin.weights[i] = 0.0f;
			}

			meshes.back().vertices.push_back(v);
		}

		// indices
		for (unsigned int i = 0; i < mesh->mNumFaces; i++)
		{
			aiFace face = mesh->mFaces[i];

			for (unsigned int j = 0; j < face.mNumIndices; j++)
			{
				meshes.back().indices.push_back(face.mIndices[j > 0 ? face.mNumIndices - j : j]);
			}
		}

		for (unsigned int b = 0; b < mesh->mNumBones; b++)
		{
			aiBone* bone = mesh->mBones[b];

			std::string name = bone->mName.C_Str();

			// assign stable global index
			uint8_t boneIndex;

			auto it = boneMap.find(name);
			if (it == boneMap.end())
			{
				boneIndex = (uint8_t)boneMap.size();
				boneMap[name] = boneIndex;
				boneOffsets.push_back(bone->mOffsetMatrix);
			}
			else
			{
				boneIndex = it->second;
			}

			// assign weights
			for (unsigned int w = 0; w < bone->mNumWeights; w++)
			{
				aiVertexWeight weight = bone->mWeights[w];

				uint32_t vertexID = weight.mVertexId;
				float wValue = weight.mWeight;

				VertexSkin& skin = meshes.back().vertices[vertexID].skin;

				// find empty slot
				for (int i = 0; i < 4; i++)
				{
					if (skin.weights[i] < 0.00001f)
					{
						skin.boneIDs[i] = boneIndex;
						skin.weights[i] = wValue;
						break;
					}
				}
			}
		}

		std::cout << "Vertex Count: " << meshes.back().vertices.size() << std::endl;
		std::cout << "Index Count: " << meshes.back().indices.size() << std::endl;
		std::cout << "Bone Count: " << mesh->mNumBones << std::endl;
	}

	std::ofstream file("model.wmesh", std::ios::binary);

	std::vector<Mesh> chunks;
	for (uint16_t m = 0; m < meshes.size(); m++)
	{
		Mesh current { meshes.at(m).materialIndex };
		std::vector<int> remap(meshes.at(m).vertices.size(), -1);

		for (size_t i = 0; i < meshes.at(m).indices.size(); i += 3) // triangles
		{
			uint32_t tri[3] = {
				meshes.at(m).indices[i],
				meshes.at(m).indices[i + 1],
				meshes.at(m).indices[i + 2]
			};

			uint32_t newVerts = 0;

			// count how many NEW vertices this triangle needs
			for (int j = 0; j < 3; j++)
			{
				if (remap[tri[j]] == -1)
					newVerts++;
			}

			if (current.indices.size() + 3 > MAX_INDICES ||
				current.vertices.size() + newVerts > MAX_VERTS)
			{
				chunks.push_back(current);

				current = Mesh { meshes.at(m).materialIndex };
				std::fill(remap.begin(), remap.end(), -1);
			}

			// add triangle
			for (int j = 0; j < 3; j++)
			{
				uint32_t oldIndex = tri[j];

				if (remap[oldIndex] == -1)
				{
					remap[oldIndex] = (int)current.vertices.size();
					current.vertices.push_back(meshes.at(m).vertices[oldIndex]);
				}

				current.indices.push_back((uint16_t)remap[oldIndex]);
			}
		}

		// last chunk
		if (!current.indices.empty())
			chunks.push_back(current);
	}

	std::cout << "Chunk Count: " << chunks.size() << std::endl;

	std::vector<uint8_t> boneParents(boneMap.size(), 0xFF);

	for (std::unordered_map<std::string, uint8_t>::iterator it = boneMap.begin();
		it != boneMap.end();
		++it)
	{
		const std::string& boneName = it->first;
		uint8_t boneIndex = it->second;

		auto parentIt = boneParentMap.find(boneName);

		if (parentIt != boneParentMap.end())
		{
			boneParents[boneIndex] = boneMap[parentIt->second];
		}
		else
		{
			boneParents[boneIndex] = 0xFF;
		}
	}

	// --- WRITE FILE ---
	char magic[]{0xDE, 0xEA, 0x7F, 0xDE, 0xFE, 0xA7, 0xED};
	file.write(magic, 7);

	int mNumMaterials = scene->mNumMaterials;
	file.write((char*)&mNumMaterials, sizeof(uint32_t));

	for (size_t i = 0; i < mNumMaterials; i++)
	{
		aiColor3D diffuse(1.0f, 1.0f, 1.0f);
		scene->mMaterials[i]->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);

		float roughness = 0.0f;
		scene->mMaterials[i]->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);

		float metallic = 0.0f;
		scene->mMaterials[i]->Get(AI_MATKEY_METALLIC_FACTOR, metallic);

		float col[]{ diffuse.r, diffuse.g, diffuse.b, roughness, metallic };
		file.write((char*)col, sizeof(float) * 5);
	}

	uint8_t boneCount = (uint8_t)boneOffsets.size();
	file.write((char*)&boneCount, sizeof(uint8_t));

	for (uint8_t i = 0; i < boneCount; i++)
	{
		file.write((char*)&boneOffsets[i], sizeof(aiMatrix4x4));
	}

	file.write((char*)boneParents.data(), boneParents.size());

	uint16_t chunkCount = (uint16_t)chunks.size();
	file.write((char*)&chunkCount, sizeof(uint16_t));

	for (auto& chunk : chunks)
	{
		Header h;
		h.materialIndex = chunk.materialIndex;
		h.vertexCount = chunk.vertices.size();
		h.indexCount = chunk.indices.size();

		file.write((char*)&h, sizeof(h));

		file.write(
			(char*)chunk.vertices.data(),
			chunk.vertices.size() * sizeof(Vertex)
		);

		file.write(
			(char*)chunk.indices.data(),
			chunk.indices.size() * sizeof(uint16_t)
		);
	}

	return 0;
}