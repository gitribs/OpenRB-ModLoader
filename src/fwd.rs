//! `SongDevice` forwards so [`crate::seq::Sequencer`] can drive the same 808/909 instances
//! [`crate::Modloader`] renders with `process_mix`.

use std::ptr::NonNull;
use std::sync::Arc;

use orb_engine::frame::{Block, BlockContext, ChannelDevice, SongDevice};
use orb_engine::samples::{Retired, SampleSet};
use orb_engine::seq::TrackMut;
use orb_engine::{tr808, tr909};

macro_rules! fwd_drum {
    ($name:ident, $ty:ty) => {
        pub struct $name(pub NonNull<$ty>);

        unsafe impl Send for $name {}

        impl SongDevice for $name {
            fn step(&mut self, tick: i32, shuffled: i32, tempo: i32) {
                SongDevice::step(unsafe { self.0.as_mut() }, tick, shuffled, tempo);
            }
            fn relocate(&mut self, tick: i32, now: i32) {
                SongDevice::relocate(unsafe { self.0.as_mut() }, tick, now);
            }
            fn start(&mut self, now: i32) {
                SongDevice::start(unsafe { self.0.as_mut() }, now);
            }
            fn stop(&mut self) {
                SongDevice::stop(unsafe { self.0.as_mut() });
            }
            fn reset(&mut self) {
                SongDevice::reset(unsafe { self.0.as_mut() });
            }
            fn silence(&mut self) {
                SongDevice::silence(unsafe { self.0.as_mut() });
            }
            fn set_song_mode(&mut self, on: bool) {
                SongDevice::set_song_mode(unsafe { self.0.as_mut() }, on);
            }
            fn data(&mut self, payload: &[u8], song_mode: bool) {
                SongDevice::data(unsafe { self.0.as_mut() }, payload, song_mode);
            }
            fn set_param(&mut self, ctrl: u8, value: u8) {
                SongDevice::set_param(unsafe { self.0.as_mut() }, ctrl, value);
            }
            fn param_message(&mut self, ctrl: u8, value: u8) {
                SongDevice::param_message(unsafe { self.0.as_mut() }, ctrl, value);
            }
            fn set_sound_model(&mut self, vintage: bool) {
                SongDevice::set_sound_model(unsafe { self.0.as_mut() }, vintage);
            }
            fn init_track(&mut self, track: &mut TrackMut) {
                SongDevice::init_track(unsafe { self.0.as_mut() }, track);
            }
            fn live(&self) -> &[u8] {
                SongDevice::live(unsafe { self.0.as_ref() })
            }
            fn pattern_params(&self) -> &[u8] {
                SongDevice::pattern_params(unsafe { self.0.as_ref() })
            }
        }

        impl ChannelDevice for $name {
            fn process(&mut self, bus: &mut Block, ctx: &BlockContext) {
                ChannelDevice::process(unsafe { self.0.as_mut() }, bus, ctx);
            }
            fn playing_pattern(&self) -> Option<u8> {
                ChannelDevice::playing_pattern(unsafe { self.0.as_ref() })
            }
            fn patterns_mut(&mut self) -> Option<&mut [u8]> {
                ChannelDevice::patterns_mut(unsafe { self.0.as_mut() })
            }
            fn stack_word(&self) -> Option<i32> {
                ChannelDevice::stack_word(unsafe { self.0.as_ref() })
            }
            fn set_samples(&mut self, samples: &Arc<SampleSet>, retired: &mut Retired) {
                ChannelDevice::set_samples(unsafe { self.0.as_mut() }, samples, retired);
            }
        }
    };
}

fwd_drum!(Fwd808, tr808::Tr808);
fwd_drum!(Fwd909, tr909::Tr909);
