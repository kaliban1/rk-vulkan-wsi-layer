
/*
 * Copyright (c) 2024 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file time_domains.hpp
 *
 * @brief Contains functions and details for the time domains per backend.
 */

#pragma once

#include <cstddef>
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <util/log.hpp>
#include "util/custom_allocator.hpp"
#include "layer/wsi_layer_experimental.hpp"
namespace wsi
{
#if VULKAN_WSI_LAYER_EXPERIMENTAL

// Predefined struct for calibrated time
struct swapchain_calibrated_time
{
   VkTimeDomainKHR time_domain;
   uint64_t offset;
};

// Base struct for swapchain time domain
class swapchain_time_domain
{
public:
   swapchain_time_domain(VkPresentStageFlagsEXT presentStages)
      : m_present_stages(presentStages)
   {
   }

   virtual swapchain_calibrated_time calibrate() = 0;

   VkPresentStageFlagsEXT get_present_stages()
   {
      return m_present_stages;
   }

private:
   VkPresentStageFlagsEXT m_present_stages;
};

class vulkan_time_domain : public swapchain_time_domain
{
public:
   vulkan_time_domain(VkPresentStageFlagsEXT presentStages, VkTimeDomainKHR time_domain)
      : swapchain_time_domain(presentStages)
      , m_time_domain(time_domain)
   {
   }

   /* The calibrate function should return a Vulkan time domain + an offset.*/
   swapchain_calibrated_time calibrate() override
   {
      return { m_time_domain, 0 };
   }

private:
   VkTimeDomainKHR m_time_domain;
};

/*  Class holding multiple time domains for a swapchain*/
class swapchain_time_domains
{
public:
   swapchain_time_domains(const util::allocator &allocator)
      : m_time_domains(allocator)
   {
   }

   util::vector<util::unique_ptr<swapchain_time_domain>> m_time_domains;

   VkResult calibrate(VkPresentStageFlagBitsEXT presentStages, swapchain_calibrated_time *calibrated_time);

   VkResult set_swapchain_time_domain_properties(VkSwapchainTimeDomainPropertiesEXT *pSwapchainTimeDomainProperties,
                                                 uint64_t *pTimeDomainsCounter);
};
#endif
}