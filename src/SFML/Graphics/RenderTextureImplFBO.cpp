////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2018 Laurent Gomila (laurent@sfml-dev.org)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Graphics/RenderTextureImplFBO.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/GLCheck.hpp>
#include <SFML/System/Mutex.hpp>
#include <SFML/System/Lock.hpp>
#include <SFML/System/Err.hpp>
#include <utility>
#include <set>


namespace
{
    // Set to track all active FBO mappings
    // This is used to free active FBOs while their owning
    // RenderTextureImplFBO is still alive
    std::set<std::map<sf::Uint64, unsigned int>*> frameBuffers;

    // Set to track all stale FBOs
    // This is used to free stale FBOs after their owning
    // RenderTextureImplFBO has already been destroyed
    // An FBO cannot be destroyed until it's containing context
    // becomes active, so the destruction of the RenderTextureImplFBO
    // has to be decoupled from the destruction of the FBOs themselves
    std::set<std::pair<sf::Uint64, unsigned int> > staleFrameBuffers;

    // Mutex to protect both active and stale frame buffer sets
    sf::Mutex mutex;

    // Callback that is called every time a context is destroyed
    void contextDestroyCallback(void* arg)
    {
        sf::Lock lock(mutex);

        sf::Uint64 contextId = sf::Context::getActiveContextId();

        // Destroy active frame buffer objects
        for (std::set<std::map<sf::Uint64, unsigned int>*>::iterator frameBuffersIter = frameBuffers.begin(); frameBuffersIter != frameBuffers.end(); ++frameBuffersIter)
        {
            for (std::map<sf::Uint64, unsigned int>::iterator iter = (*frameBuffersIter)->begin(); iter != (*frameBuffersIter)->end(); ++iter)
            {
                if (iter->first == contextId)
                {
                    GLuint frameBuffer = static_cast<GLuint>(iter->second);
                    glCheck(GLEXT_glDeleteFramebuffers(1, &frameBuffer));

                    // Erase the entry from the RenderTextureImplFBO's map
                    (*frameBuffersIter)->erase(iter);

                    break;
                }
            }
        }

        // Destroy stale frame buffer objects
        for (std::set<std::pair<sf::Uint64, unsigned int> >::iterator iter = staleFrameBuffers.begin(); iter != staleFrameBuffers.end(); ++iter)
        {
            if (iter->first == contextId)
            {
                GLuint frameBuffer = static_cast<GLuint>(iter->second);
                glCheck(GLEXT_glDeleteFramebuffers(1, &frameBuffer));
            }
        }
    }
}


namespace sf
{
namespace priv
{
////////////////////////////////////////////////////////////
RenderTextureImplFBO::RenderTextureImplFBO() :
m_context    (NULL),
m_depthBuffer(0),
m_textureId  (0)
{
    Lock lock(mutex);

    // Register the context destruction callback
    registerContextDestroyCallback(contextDestroyCallback, 0);

    // Insert the new frame buffer mapping into the set of all active mappings
    frameBuffers.insert(&m_frameBuffers);
}


////////////////////////////////////////////////////////////
RenderTextureImplFBO::~RenderTextureImplFBO()
{
    Lock lock(mutex);

    // Remove the frame buffer mapping from the set of all active mappings
    frameBuffers.erase(&m_frameBuffers);

    // Destroy the depth buffer
    if (m_depthBuffer)
    {
        GLuint depthBuffer = static_cast<GLuint>(m_depthBuffer);
        glCheck(GLEXT_glDeleteRenderbuffers(1, &depthBuffer));
    }

    // Move all frame buffer objects to stale set
    for (std::map<Uint64, unsigned int>::iterator iter = m_frameBuffers.begin(); iter != m_frameBuffers.end(); ++iter)
        staleFrameBuffers.insert(std::make_pair(iter->first, iter->second));

    // Clean up FBOs
    contextDestroyCallback(0);

    // Delete the backup context if we had to create one
    delete m_context;
}


////////////////////////////////////////////////////////////
bool RenderTextureImplFBO::isAvailable()
{
    TransientContextLock lock;

    // Make sure that extensions are initialized
    priv::ensureExtensionsInit();

    return GLEXT_framebuffer_object != 0;
}


////////////////////////////////////////////////////////////
void RenderTextureImplFBO::unbind()
{
    glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, 0));
}


////////////////////////////////////////////////////////////
bool RenderTextureImplFBO::create(unsigned int width, unsigned int height, unsigned int textureId, bool depthBuffer)
{
    // Create the depth buffer if requested
    if (depthBuffer)
    {
        TransientContextLock lock;

        GLuint depth = 0;
        glCheck(GLEXT_glGenRenderbuffers(1, &depth));
        m_depthBuffer = static_cast<unsigned int>(depth);
        if (!m_depthBuffer)
        {
            err() << "Impossible to create render texture (failed to create the attached depth buffer)" << std::endl;
            return false;
        }
        glCheck(GLEXT_glBindRenderbuffer(GLEXT_GL_RENDERBUFFER, m_depthBuffer));
        glCheck(GLEXT_glRenderbufferStorage(GLEXT_GL_RENDERBUFFER, GLEXT_GL_DEPTH_COMPONENT, width, height));
    }

    // Save our texture ID in order to be able to attach it to an FBO at any time
    m_textureId = textureId;

    // We can't create an FBO now if there is no active context
    if (!Context::getActiveContextId())
        return true;

#ifndef SFML_OPENGL_ES

    // Save the current bindings so we can restore them after we are done
    GLint readFramebuffer = 0;
    GLint drawFramebuffer = 0;

    glCheck(glGetIntegerv(GLEXT_GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer));
    glCheck(glGetIntegerv(GLEXT_GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer));

    if (createFrameBuffer())
    {
        // Restore previously bound framebuffers
        glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_READ_FRAMEBUFFER, readFramebuffer));
        glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_DRAW_FRAMEBUFFER, drawFramebuffer));

        return true;
    }

#else

    // Save the current binding so we can restore them after we are done
    GLint frameBuffer = 0;

    glCheck(glGetIntegerv(GLEXT_GL_FRAMEBUFFER_BINDING, &frameBuffer));

    if (createFrameBuffer())
    {
        // Restore previously bound framebuffer
        glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, frameBuffer));

        return true;
    }

#endif

    return false;
}


////////////////////////////////////////////////////////////
bool RenderTextureImplFBO::createFrameBuffer()
{
    // Create the framebuffer object
    GLuint frameBuffer = 0;
    glCheck(GLEXT_glGenFramebuffers(1, &frameBuffer));

    if (!frameBuffer)
    {
        err() << "Impossible to create render texture (failed to create the frame buffer object)" << std::endl;
        return false;
    }
    glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, frameBuffer));

    // Link the texture to the frame buffer
    glCheck(GLEXT_glFramebufferTexture2D(GLEXT_GL_FRAMEBUFFER, GLEXT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_textureId, 0));

    // Link the depth renderbuffer to the frame buffer
    if (m_depthBuffer)
    {
        glCheck(GLEXT_glFramebufferRenderbuffer(GLEXT_GL_FRAMEBUFFER, GLEXT_GL_DEPTH_ATTACHMENT, GLEXT_GL_RENDERBUFFER, m_depthBuffer));
    }

    // A final check, just to be sure...
    GLenum status;
    glCheck(status = GLEXT_glCheckFramebufferStatus(GLEXT_GL_FRAMEBUFFER));
    if (status != GLEXT_GL_FRAMEBUFFER_COMPLETE)
    {
        glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, 0));
        glCheck(GLEXT_glDeleteFramebuffers(1, &frameBuffer));
        err() << "Impossible to create render texture (failed to link the target texture to the frame buffer)" << std::endl;
        return false;
    }

    Lock lock(mutex);

    // Insert the FBO into our map
    m_frameBuffers.insert(std::make_pair(Context::getActiveContextId(), static_cast<unsigned int>(frameBuffer)));

    return true;
}


////////////////////////////////////////////////////////////
bool RenderTextureImplFBO::activate(bool active)
{
    // Unbind the FBO if requested
    if (!active)
    {
        glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, 0));
        return true;
    }

    Uint64 contextId = Context::getActiveContextId();

    // In the odd case we have to activate and there is no active
    // context yet, we have to create one
    if (!contextId)
    {
        if (!m_context)
            m_context = new Context;

        m_context->setActive(true);

        contextId = Context::getActiveContextId();

        if (!contextId)
        {
            err() << "Impossible to activate render texture (failed to create backup context)" << std::endl;

            return false;
        }
    }

    // Lookup the FBO corresponding to the currently active context
    // If none is found, there is no FBO corresponding to the
    // currently active context so we will have to create a new FBO
    {
        Lock lock(mutex);

        std::map<Uint64, unsigned int>::iterator iter = m_frameBuffers.find(contextId);

        if (iter != m_frameBuffers.end())
        {
            glCheck(GLEXT_glBindFramebuffer(GLEXT_GL_FRAMEBUFFER, iter->second));

            return true;
        }
    }

    return createFrameBuffer();
}


////////////////////////////////////////////////////////////
void RenderTextureImplFBO::updateTexture(unsigned int)
{
    // Nothing to do
}

} // namespace priv

} // namespace sf
