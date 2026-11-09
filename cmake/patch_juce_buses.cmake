# Output / 808 Mix / 909 Mix are VST3 kMain. Drum-slot extras are kAux, and AU
# 0-channel is a valid disable. Mix omit is a panel control, not bus state.

function(orb_replace_in_file file old new already)
    if(NOT EXISTS "${file}")
        message(WARNING "OpenRB: missing ${file}")
        return()
    endif()
    file(READ "${file}" contents)
    if(contents MATCHES "${already}")
        return()
    endif()
    string(FIND "${contents}" "${old}" pos)
    if(pos EQUAL -1)
        message(WARNING "OpenRB: could not patch ${file} (JUCE text changed)")
        return()
    endif()
    string(REPLACE "${old}" "${new}" contents "${contents}")
    file(WRITE "${file}" "${contents}")
    message(STATUS "OpenRB: patched ${file}")
endfunction()

if(NOT DEFINED juce_SOURCE_DIR)
    set(juce_SOURCE_DIR "${JUCE_SOURCE_DIR}")
endif()

orb_replace_in_file(
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp"
    "                   #if JucePlugin_IsSynth
                    return Vst::kMain;
                   #else
                    return isFirstBus ? Vst::kMain : Vst::kAux;
                   #endif"
    "                    // OpenRB: Output, 808 Mix, and 909 Mix are kMain. Drum slots stay kAux.
                    return (index < 3) ? Vst::kMain : Vst::kAux;"
    "OpenRB: Output, 808 Mix, and 909 Mix are kMain"
)

orb_replace_in_file(
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp"
    "                    // OpenRB: extra synth outs as kAux so hosts deactivate them on disconnect.
                    return isFirstBus ? Vst::kMain : Vst::kAux;"
    "                    // OpenRB: Output, 808 Mix, and 909 Mix are kMain. Drum slots stay kAux.
                    return (index < 3) ? Vst::kMain : Vst::kAux;"
    "OpenRB: Output, 808 Mix, and 909 Mix are kMain"
)

orb_replace_in_file(
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_AU_1.mm"
    "        if (set == AudioChannelSet())
            return kAudioUnitErr_FormatNotSupported;

        const auto err = MusicDeviceBase::ChangeStreamFormat (inScope, inElement, inPrevFormat, inNewFormat);

        if (err == noErr)
            currentTag = CoreAudioLayouts::toCoreAudio (set);

        return err;"
    "        // OpenRB: 0-channel is a valid disable. Empty AudioChannelSet is both
        // \"disabled\" and \"unsupported\", so rejecting all empty sets blocks hosts
        // from deactivating aux buses.
        if (set == AudioChannelSet() && newNumChannels != 0)
            return kAudioUnitErr_FormatNotSupported;

        const auto err = MusicDeviceBase::ChangeStreamFormat (inScope, inElement, inPrevFormat, inNewFormat);

        if (err == noErr)
        {
            currentTag = CoreAudioLayouts::toCoreAudio (set);

            if (info.kind != BusKind::wrapperOnly)
            {
                juceFilter->setChannelLayoutOfBus (info.isInput, info.busNr, set);

                // OpenRB: keep AU element names in sync when a host enables an aux.
                if (auto* namedBus = juceFilter->getBus (info.isInput, info.busNr))
                {
                    IOElement (inScope, inElement).SetName ((CFStringRef) juceStringToNS (namedBus->getName()));
                    PropertyChanged (kAudioUnitProperty_ElementName, inScope, inElement);
                }
            }
        }

        return err;"
    "OpenRB: 0-channel is a valid disable"
)

# Already-patched trees have the 0-channel fix without element names.
orb_replace_in_file(
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_AU_1.mm"
    "            if (info.kind != BusKind::wrapperOnly)
                juceFilter->setChannelLayoutOfBus (info.isInput, info.busNr, set);"
    "            if (info.kind != BusKind::wrapperOnly)
            {
                juceFilter->setChannelLayoutOfBus (info.isInput, info.busNr, set);

                // OpenRB: keep AU element names in sync when a host enables an aux.
                if (auto* namedBus = juceFilter->getBus (info.isInput, info.busNr))
                {
                    IOElement (inScope, inElement).SetName ((CFStringRef) juceStringToNS (namedBus->getName()));
                    PropertyChanged (kAudioUnitProperty_ElementName, inScope, inElement);
                }
            }"
    "OpenRB: keep AU element names"
)

orb_replace_in_file(
    "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_AU_1.mm"
    "        // is this bus activated?
        if (numChannels == 0)
            return noErr;

        auto& element = IOElement (isInput ? kAudioUnitScope_Input :  kAudioUnitScope_Output, (UInt32) busNr);

        element.SetName ((CFStringRef) juceStringToNS (juceFilter->getBus (isInput, busNr)->getName()));"
    "        auto& element = IOElement (isInput ? kAudioUnitScope_Input :  kAudioUnitScope_Output, (UInt32) busNr);

        // OpenRB: publish bus names even when the aux starts disabled (0 channels).
        // Otherwise hosts never see ElementName and label mixer strips with the plugin name.
        element.SetName ((CFStringRef) juceStringToNS (juceFilter->getBus (isInput, busNr)->getName()));

        if (numChannels == 0)
            return noErr;"
    "OpenRB: publish bus names even when the aux starts disabled"
)
