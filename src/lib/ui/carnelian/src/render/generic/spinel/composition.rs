// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{ops::RangeBounds, ptr, slice};

use euclid::default::{Rect, Size2D};
use spinel_rs_sys::*;

use crate::{
    color::Color,
    drawing::DisplayRotation,
    render::generic::{
        spinel::{init, InnerContext, Spinel},
        BlendMode, Composition, Fill, FillRule, Layer, Style,
    },
};

fn group_layers(
    spn_styling: SpnStyling,
    top_group: SpnGroupId,
    layers: &[Layer<Spinel>],
    layer_id_start: u32,
) {
    fn cmds_len(style: &Style) -> usize {
        let fill_rule_len = match style.fill_rule {
            FillRule::NonZero => 1,
            FillRule::EvenOdd => 1,
            FillRule::WholeTile => 0,
        };
        let fill_len = match style.fill {
            Fill::Solid(..) => 3,
        };
        let blend_mode_len = match style.blend_mode {
            BlendMode::Over => 1,
        };

        1 + fill_rule_len + fill_len + blend_mode_len
    }

    for (i, Layer { style, .. }) in layers.iter().enumerate() {
        let cmds = unsafe {
            let len = cmds_len(style);
            let data = init(|ptr| {
                spn!(spn_styling_group_layer(
                    spn_styling,
                    top_group,
                    layer_id_start + i as u32,
                    len as u32,
                    ptr
                ))
            });
            slice::from_raw_parts_mut(data, len)
        };

        cmds[0] = SpnCommand::SpnStylingOpcodeCoverWipZero;
        let mut cursor = 1;

        match style.fill_rule {
            FillRule::NonZero => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeCoverNonzero;
                cursor += 1;
            }
            FillRule::EvenOdd => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeCoverEvenodd;
                cursor += 1;
            }
            FillRule::WholeTile => {}
        }

        match style.fill {
            Fill::Solid(color) => {
                let color = color.to_linear_premult_rgba();
                unsafe {
                    spn_styling_layer_fill_rgba_encoder(&mut cmds[cursor], color.as_ptr());
                }
                cursor += 3;
            }
        }

        match style.blend_mode {
            BlendMode::Over => {
                cmds[cursor] = SpnCommand::SpnStylingOpcodeBlendOver;
            }
        }
    }
}

#[derive(Clone, Debug)]
pub struct SpinelComposition {
    pub(crate) layers: Vec<Layer<Spinel>>,
    pub(crate) background_color: [f32; 4],
}

impl SpinelComposition {
    pub(crate) fn set_up_spn_composition(
        &self,
        context: &InnerContext,
        raster_builder: SpnRasterBuilder,
        composition: SpnComposition,
        size: Size2D<u32>,
        display_rotation: DisplayRotation,
        clip: Rect<u32>,
    ) {
        unsafe {
            let clip = [clip.min_x(), clip.min_y(), clip.max_x(), clip.max_y()];
            spn!(spn_composition_reset(composition));
            spn!(spn_composition_set_clip(composition, clip.as_ptr(),));
        }

        for (i, Layer { raster, .. }) in self.layers.iter().enumerate() {
            for (paths, txty) in raster.rasters.iter() {
                unsafe {
                    spn!(spn_raster_builder_begin(raster_builder));
                }

                for (path, transform) in paths.iter() {
                    const SPINEL_TRANSFORM_MULTIPLIER: f32 = 32.0;

                    let transform = transform
                        .then_translate(*txty)
                        .then(&display_rotation.transform(&size.to_f32()));
                    let transform = SpnTransform {
                        sx: transform.m11 * SPINEL_TRANSFORM_MULTIPLIER,
                        shx: transform.m21 * SPINEL_TRANSFORM_MULTIPLIER,
                        tx: transform.m31 * SPINEL_TRANSFORM_MULTIPLIER,
                        shy: transform.m12 * SPINEL_TRANSFORM_MULTIPLIER,
                        sy: transform.m22 * SPINEL_TRANSFORM_MULTIPLIER,
                        ty: transform.m32 * SPINEL_TRANSFORM_MULTIPLIER,
                        w0: 0.0,
                        w1: 0.0,
                    };
                    let clip = SpnClip {
                        x0: std::f32::MIN,
                        y0: std::f32::MIN,
                        x1: std::f32::MAX,
                        y1: std::f32::MAX,
                    };

                    unsafe {
                        spn!(spn_raster_builder_add(
                            raster_builder,
                            &*path.path,
                            ptr::null_mut(),
                            &transform,
                            ptr::null_mut(),
                            &clip,
                            1,
                        ));
                    }
                }

                let raster =
                    unsafe { init(|ptr| spn!(spn_raster_builder_end(raster_builder, ptr))) };

                unsafe {
                    spn!(spn_composition_place(composition, &raster, &(i as u32), ptr::null(), 1,));
                }

                context.get_checked().map(|context| unsafe {
                    spn!(spn_raster_release(context, &raster as *const _, 1))
                });
            }
        }
    }

    pub(crate) fn spn_styling(
        &self,
        context: &InnerContext,
        needs_linear_to_srgb_opcode: bool,
    ) -> Option<SpnStyling> {
        const PARENTS: u32 = 0; // spn_styling_group_parents
        const ENTER_CMDS: u32 = 1; // spn_styling_group_enter
        const GROUP_SIZE: u32 = 6; // spn_styling_group_alloc
        const MAX_LAYER_CMDS: u32 = 6; // spn_styling_group_layer
        let leave_cmds: u32 = if needs_linear_to_srgb_opcode { 5 } else { 4 }; // spn_styling_group_leave

        let len = self.layers.len() as u32;
        let styling_len = len * MAX_LAYER_CMDS + PARENTS + ENTER_CMDS + leave_cmds + GROUP_SIZE;
        let spn_styling = context.get_checked().map(|context| unsafe {
            init(|ptr| spn!(spn_styling_create(context, ptr, len, styling_len)))
        })?;

        let top_group = unsafe { init(|ptr| spn!(spn_styling_group_alloc(spn_styling, ptr))) };

        unsafe {
            spn!(spn_styling_group_parents(spn_styling, top_group, PARENTS, ptr::null_mut()));
            if len != 0 {
                spn!(spn_styling_group_range_lo(spn_styling, top_group, 0));
                spn!(spn_styling_group_range_hi(spn_styling, top_group, len - 1));
            }
        }

        let cmds_enter = unsafe {
            let data =
                init(|ptr| spn!(spn_styling_group_enter(spn_styling, top_group, ENTER_CMDS, ptr)));
            slice::from_raw_parts_mut(data, 1)
        };
        cmds_enter[0] = SpnCommand::SpnStylingOpcodeColorAccZero;

        let cmds_leave = unsafe {
            let data =
                init(|ptr| spn!(spn_styling_group_leave(spn_styling, top_group, leave_cmds, ptr)));
            slice::from_raw_parts_mut(data, leave_cmds as usize)
        };
        unsafe {
            spn_styling_background_over_encoder(&mut cmds_leave[0], self.background_color.as_ptr());
        }
        if needs_linear_to_srgb_opcode {
            cmds_leave[3] = SpnCommand::SpnStylingOpcodeColorAccLinearToSrgb;
            cmds_leave[4] = SpnCommand::SpnStylingOpcodeColorAccStoreToSurface;
        } else {
            cmds_leave[3] = SpnCommand::SpnStylingOpcodeColorAccStoreToSurface;
        }

        group_layers(spn_styling, top_group, &self.layers, 0);

        unsafe {
            spn!(spn_styling_seal(spn_styling));
        }

        Some(spn_styling)
    }
}

impl Composition<Spinel> for SpinelComposition {
    fn new(background_color: Color) -> Self {
        Self { layers: vec![], background_color: background_color.to_linear_premult_rgba() }
    }

    fn with_layers(
        layers: impl IntoIterator<Item = Layer<Spinel>>,
        background_color: Color,
    ) -> Self {
        Self {
            layers: layers.into_iter().collect(),
            background_color: background_color.to_linear_premult_rgba(),
        }
    }

    fn clear(&mut self) {
        self.layers.clear();
    }

    fn replace<R, I>(&mut self, range: R, with: I)
    where
        R: RangeBounds<usize>,
        I: IntoIterator<Item = Layer<Spinel>>,
    {
        self.layers.splice(range, with);
    }
}
