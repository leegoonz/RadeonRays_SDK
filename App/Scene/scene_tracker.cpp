#include "Scene/scene_tracker.h"
#include "Scene/scene.h"
#include "CLW/clwscene.h"
#include "perspective_camera.h"

using namespace FireRays;


namespace Baikal
{
    ClwScene& SceneTracker::CompileScene(Scene const& scene) const
    {
        auto iter = m_scene_cache.find(&scene);

        if (iter == m_scene_cache.cend())
        {
            ClwScene out;
            RecompileFull(scene, out);
            m_scene_cache.insert(std::make_pair(&scene, out));
            return m_scene_cache[&scene];
        }
        else
        {
            auto& out = iter->second;

            if (scene.dirty() == Scene::DirtyFlags::kCamera)
            {
                // Update camera data
                m_context.WriteBuffer(0, out.camera, scene.camera_.get(), 1);
            }

            return out;
        }
    }

    void SceneTracker::RecompileFull(Scene const& scene, ClwScene& out) const
    {
        m_vidmem_usage = 0;

        // Create static buffers
        out.camera = m_context.CreateBuffer<PerspectiveCamera>(1, CL_MEM_READ_ONLY |  CL_MEM_COPY_HOST_PTR, scene.camera_.get());

        // Vertex, normal and uv data
        out.vertices = m_context.CreateBuffer<float3>(scene.vertices_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.vertices_[0]);
        m_vidmem_usage += scene.vertices_.size() * sizeof(float3);

        out.normals = m_context.CreateBuffer<float3>(scene.normals_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.normals_[0]);
        m_vidmem_usage += scene.normals_.size() * sizeof(float3);

        out.uvs = m_context.CreateBuffer<float2>(scene.uvs_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.uvs_[0]);
        m_vidmem_usage += scene.uvs_.size() * sizeof(float2);

        // Index data
        out.indices = m_context.CreateBuffer<int>(scene.indices_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.indices_[0]);
        m_vidmem_usage += scene.indices_.size() * sizeof(int);

        // Shapes
        out.shapes = m_context.CreateBuffer<Scene::Shape>(scene.shapes_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.shapes_[0]);
        m_vidmem_usage += scene.shapes_.size() * sizeof(Scene::Shape);

        // Material IDs
        out.materialids = m_context.CreateBuffer<int>(scene.materialids_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.materialids_[0]);
        m_vidmem_usage += scene.materialids_.size() * sizeof(int);

        // Material descriptions
        out.materials = m_context.CreateBuffer<Scene::Material>(scene.materials_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.materials_[0]);
        m_vidmem_usage += scene.materials_.size() * sizeof(Scene::Material);

        // Bake textures
        BakeTextures(scene, out);

        // Emissives
        if (scene.emissives_.size() > 0)
        {
            out.emissives = m_context.CreateBuffer<Scene::Emissive>(scene.emissives_.size(), CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (void*)&scene.emissives_[0]);
            out.numemissive = scene.emissives_.size();
            m_vidmem_usage += scene.emissives_.size() * sizeof(Scene::Emissive);
        }
        else
        {
            out.emissives = m_context.CreateBuffer<Scene::Emissive>(1, CL_MEM_READ_ONLY);
            out.numemissive = 0;
            m_vidmem_usage += sizeof(Scene::Emissive);
        }

        //Volume vol = {1, 0, 0, 0, {0.9f, 0.6f, 0.9f}, {5.1f, 1.8f, 5.1f}, {0.0f, 0.0f, 0.0f}};
        Scene::Volume vol = { 1, 0, 0, 0,{	1.2f, 0.4f, 1.2f },{ 5.1f, 4.8f, 5.1f },{ 0.0f, 0.0f, 0.0f } };

        out.volumes = m_context.CreateBuffer<Scene::Volume>(1, CL_MEM_READ_ONLY, &vol);

        out.envmapmul = scene.envmapmul_;
        out.envmapidx = scene.envidx_;

        std::cout << "Vidmem usage (data): " << m_vidmem_usage / (1024 * 1024) << "Mb\n";
        std::cout << "Polygon count " << scene.indices_.size() / 3 << "\n";
    }

    void SceneTracker::BakeTextures(Scene const& scene, ClwScene& out) const
    {
        if (scene.textures_.size() > 0)
        {
            // Evaluate data size
            size_t datasize = 0;
            for (auto iter = scene.textures_.cbegin(); iter != scene.textures_.cend(); ++iter)
            {
                datasize += iter->size;
            }

            // Texture descriptors
            out.textures = m_context.CreateBuffer<Scene::Texture>(scene.textures_.size(), CL_MEM_READ_ONLY);
            m_vidmem_usage += scene.textures_.size() * sizeof(Scene::Texture);

            // Texture data
            out.texturedata = m_context.CreateBuffer<char>(datasize, CL_MEM_READ_ONLY);

            // Map both buffers
            Scene::Texture* mappeddesc = nullptr;
            char* mappeddata = nullptr;
            Scene::Texture* mappeddesc_orig = nullptr;
            char* mappeddata_orig = nullptr;

            m_context.MapBuffer(0, out.textures, CL_MAP_WRITE, &mappeddesc).Wait();
            m_context.MapBuffer(0, out.texturedata, CL_MAP_WRITE, &mappeddata).Wait();

            // Save them for unmap
            mappeddesc_orig = mappeddesc;
            mappeddata_orig = mappeddata;

            // Write data into both buffers
            int current_offset = 0;
            for (auto iter = scene.textures_.cbegin(); iter != scene.textures_.cend(); ++iter)
            {
                // Copy texture desc
                Scene::Texture texture = *iter;

                // Write data into the buffer
                memcpy(mappeddata, scene.texturedata_[texture.dataoffset].get(), texture.size);
                m_vidmem_usage += texture.size;

                // Adjust offset in the texture
                texture.dataoffset = current_offset;

                // Copy desc into the buffer
                *mappeddesc = texture;

                // Adjust offset
                current_offset += texture.size;

                // Adjust data pointer
                mappeddata += texture.size;

                // Adjust descriptor pointer
                ++mappeddesc;
            }

            m_context.UnmapBuffer(0, out.textures, mappeddesc_orig).Wait();
            m_context.UnmapBuffer(0, out.texturedata, mappeddata_orig).Wait();
        }
        else
        {
            // Create stub
            out.textures = m_context.CreateBuffer<Scene::Texture>(1, CL_MEM_READ_ONLY);
            m_vidmem_usage += sizeof(Scene::Texture);

            // Texture data
            out.texturedata = m_context.CreateBuffer<char>(1, CL_MEM_READ_ONLY);
            m_vidmem_usage += 1;
        }
    }
}
