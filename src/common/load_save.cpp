/* Copyright 2013-2016 Matt Tytel
 *
 * helm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * helm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with helm.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "load_save.h"
#include "JuceHeader.h"
#include "helm_common.h"
#include "midi_manager.h"

#define LINUX_FACTORY_PATCH_DIRECTORY "/usr/share/helm/patches"
#define USER_BANK_NAME "User Patches"
#define LINUX_BANK_DIRECTORY "~/.helm/patches"

namespace {

  String createPatchLicense(String author) {
    return "Patch (c) by " + author +
           ".  This patch is licensed under a " +
           "Creative Commons Attribution 4.0 International License.  " +
           "You should have received a copy of the license along with this " +
           "work.  If not, see <http://creativecommons.org/licenses/by/4.0/>.";
  }

  const String DEFAULT_USER_FOLDERS[] = { "Lead", "Keys", "Pad", "Bass", "SFX" };
} // namespace

var LoadSave::stateToVar(mopo::HelmEngine* synth,
                         std::map<std::string, String>& save_info,
                         const CriticalSection& critical_section) {
  mopo::control_map controls = synth->getControls();
  DynamicObject* settings_object = new DynamicObject();

  ScopedLock lock(critical_section);
  for (auto control : controls)
    settings_object->setProperty(String(control.first), control.second->value());

  std::set<mopo::ModulationConnection*> modulations = synth->getModulationConnections();
  Array<var> modulation_states;
  for (mopo::ModulationConnection* connection: modulations) {
    DynamicObject* mod_object = new DynamicObject();
    mod_object->setProperty("source", connection->source.c_str());
    mod_object->setProperty("destination", connection->destination.c_str());
    mod_object->setProperty("amount", connection->amount.value());
    modulation_states.add(mod_object);
  }

  settings_object->setProperty("modulations", modulation_states);

  DynamicObject* state_object = new DynamicObject();
  String author = save_info["author"];
  state_object->setProperty("license", createPatchLicense(author));
  state_object->setProperty("synth_version", ProjectInfo::versionString);
  state_object->setProperty("patch_name", save_info["patch_name"]);
  state_object->setProperty("folder_name", save_info["folder_name"]);
  state_object->setProperty("author", author);
  state_object->setProperty("settings", settings_object);
  return state_object;
}

void LoadSave::loadControls(mopo::HelmEngine* synth,
                            const NamedValueSet& properties) {
  mopo::control_map controls = synth->getControls();
  for (auto control : controls) {
    String name = control.first;
    if (properties.contains(name)) {
      mopo::mopo_float value = properties[name];
      control.second->set(value);
    }
    else {
      mopo::ValueDetails details = mopo::Parameters::getDetails(name.toStdString());
      control.second->set(details.default_value);
    }
  }
}

void LoadSave::loadModulations(mopo::HelmEngine* synth,
                               const Array<var>* modulations) {
  synth->clearModulations();
  var* modulation = modulations->begin();

  for (; modulation != modulations->end(); ++modulation) {
    DynamicObject* mod = modulation->getDynamicObject();
    std::string source = mod->getProperty("source").toString().toStdString();
    std::string destination = mod->getProperty("destination").toString().toStdString();
    mopo::ModulationConnection* connection = new mopo::ModulationConnection(source, destination);
    connection->amount.set(mod->getProperty("amount"));
    synth->connectModulation(connection);
  }
}


void LoadSave::loadSaveState(std::map<std::string, String>& state,
                             const NamedValueSet& properties) {
  if (properties.contains("author"))
    state["author"] = properties["author"];
  if (properties.contains("patch_name"))
    state["patch_name"] = properties["patch_name"];
  if (properties.contains("folder_name"))
    state["folder_name"] = properties["folder_name"];
}

void LoadSave::varToState(mopo::HelmEngine* synth,
                          std::map<std::string, String>& save_info,
                          var state) {
  if (!state.isObject())
    return;

  DynamicObject* object_state = state.getDynamicObject();
  NamedValueSet properties = object_state->getProperties();

  // Version 0.4.1 was the last build before we saved the version number.
  String version = "0.4.1";
  if (properties.contains("synth_version"))
    version = properties["synth_version"];

  // After 0.4.1 there was a patch file restructure.
  if (compareVersionStrings(version, "0.4.1") <= 0) {
    NamedValueSet new_properties;
    new_properties.set("settings", object_state);
    properties = new_properties;
  }

  var settings = properties["settings"];
  DynamicObject* settings_object = settings.getDynamicObject();
  NamedValueSet settings_properties = settings_object->getProperties();
  Array<var>* modulations = settings_properties["modulations"].getArray();

  // After 0.5.0 mixer was added and osc_mix was removed. And scaling of oscillators was changed.
  if (compareVersionStrings(version, "0.5.0") <= 0) {

    // Fix control control values.
    if (settings_properties.contains("osc_mix")) {
      mopo::mopo_float osc_mix = settings_properties["osc_mix"];
      settings_properties.set("osc_1_volume", sqrt(1.0f - osc_mix));
      settings_properties.set("osc_2_volume", sqrt(osc_mix));
      settings_properties.remove("osc_mix");
    }

    // Fix modulation routing.
    var* modulation = modulations->begin();
    Array<var> old_modulations;
    Array<DynamicObject*> new_modulations;
    for (; modulation != modulations->end(); ++modulation) {
      DynamicObject* mod = modulation->getDynamicObject();
      String destination = mod->getProperty("destination").toString();

      if (destination == "osc_mix") {
        String source = mod->getProperty("source").toString();
        mopo::mopo_float amount = mod->getProperty("amount");
        old_modulations.add(mod);

        DynamicObject* osc_1_mod = new DynamicObject();
        osc_1_mod->setProperty("source", source);
        osc_1_mod->setProperty("destination", "osc_1_volume");
        osc_1_mod->setProperty("amount", -amount);
        new_modulations.add(osc_1_mod);

        DynamicObject* osc_2_mod = new DynamicObject();
        osc_2_mod->setProperty("source", source);
        osc_2_mod->setProperty("destination", "osc_2_volume");
        osc_2_mod->setProperty("amount", amount);
        new_modulations.add(osc_2_mod);
      }
    }

    for (var old_modulation : old_modulations)
      modulations->removeFirstMatchingValue(old_modulation);

    for (DynamicObject* modulation : new_modulations)
      modulations->add(modulation);
  }

  loadControls(synth, settings_properties);
  loadModulations(synth, modulations);
  loadSaveState(save_info, properties);
}

String LoadSave::getAuthor(var state) {
  if (!state.isObject())
    return "";

  DynamicObject* object_state = state.getDynamicObject();
  NamedValueSet properties = object_state->getProperties();
  if (properties.contains("author"))
    return properties["author"];
  return "";
}

String LoadSave::getLicense(var state) {
  if (!state.isObject())
    return "";

  DynamicObject* object_state = state.getDynamicObject();
  NamedValueSet properties = object_state->getProperties();
  if (properties.contains("license"))
    return properties["license"];
  return "";
}

File LoadSave::getConfigFile() {
  PropertiesFile::Options config_options;
  config_options.applicationName = "Helm";
  config_options.osxLibrarySubFolder = "Application Support";
  config_options.filenameSuffix = "config";

#ifdef LINUX
  config_options.folderName = "." + String(ProjectInfo::projectName).toLowerCase();
#else
  config_options.folderName = String(ProjectInfo::projectName).toLowerCase();
#endif

  return config_options.getDefaultFile();
}

var LoadSave::getConfigVar() {
  File config_file = getConfigFile();

  var config_state;
  if (!JSON::parse(config_file.loadFileAsString(), config_state).wasOk())
    return var();

  if (!config_state.isObject())
    return var();

  return config_state;
}

void LoadSave::saveVarToConfig(var config_state) {
  if (!isInstalled())
    return;

  File config_file = getConfigFile();

  if (!config_file.exists())
    config_file.create();
  config_file.replaceWithText(JSON::toString(config_state));
}

void LoadSave::saveVersionConfig() {
  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();
  config_object->setProperty("synth_version", ProjectInfo::versionString);
  saveVarToConfig(config_object);
}

void LoadSave::saveUpdateCheckConfig(bool check_for_updates) {
  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();
  config_object->setProperty("check_for_updates", check_for_updates);
  saveVarToConfig(config_object);
}

void LoadSave::saveAnimateWidgets(bool animate_widgets) {
  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();
  config_object->setProperty("animate_widgets", animate_widgets);
  saveVarToConfig(config_object);
}

void LoadSave::saveLayoutConfig(mopo::StringLayout* layout) {
  if (layout == nullptr)
    return;

  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();
  DynamicObject* layout_object = new DynamicObject();
  String chromatic_layout;
  wchar_t up_key = L'\0';
  wchar_t down_key = L'\0';

  if (layout) {
    chromatic_layout = String(layout->getLayout().data());
    up_key = layout->getUpKey();
    down_key = layout->getDownKey();
  }
  else {
    chromatic_layout = String(getComputerKeyboardLayout().data());
    std::pair<wchar_t, wchar_t> octave_controls = getComputerKeyboardOctaveControls();
    down_key = octave_controls.first;
    up_key = octave_controls.second;
  }

  layout_object->setProperty("chromatic_layout", chromatic_layout);
  layout_object->setProperty("octave_up", String() + up_key);
  layout_object->setProperty("octave_down", String() + down_key);
  config_object->setProperty("keyboard_layout", layout_object);
  saveVarToConfig(config_object);
}

void LoadSave::saveMidiMapConfig(MidiManager* midi_manager) {
  MidiManager::midi_map midi_learn_map = midi_manager->getMidiLearnMap();
  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();

  Array<var> midi_learn_object;
  for (auto midi_mapping : midi_learn_map) {
    DynamicObject* midi_map_object = new DynamicObject();
    Array<var> midi_destinations_object;

    midi_map_object->setProperty("source", midi_mapping.first);

    for (auto midi_destination : midi_mapping.second) {
      DynamicObject* midi_destination_object = new DynamicObject();

      midi_destination_object->setProperty("destination", String(midi_destination.first));
      midi_destination_object->setProperty("min_range", midi_destination.second.first);
      midi_destination_object->setProperty("max_range", midi_destination.second.second);
      midi_destinations_object.add(midi_destination_object);
    }

    midi_map_object->setProperty("destinations", midi_destinations_object);
    midi_learn_object.add(midi_map_object);
  }

  config_object->setProperty("midi_learn", midi_learn_object);
  saveVarToConfig(config_object);
}

void LoadSave::loadConfig(MidiManager* midi_manager, mopo::StringLayout* layout) {
  var config_var = getConfigVar();
  if (!config_var.isObject())
    config_var = new DynamicObject();

  DynamicObject* config_object = config_var.getDynamicObject();
  NamedValueSet config_properties = config_object->getProperties();

  // Computer Keyboard Layout
  if (layout) {
    layout->setLayout(getComputerKeyboardLayout());
    std::pair<wchar_t, wchar_t> octave_controls = getComputerKeyboardOctaveControls();
    layout->setDownKey(octave_controls.first);
    layout->setUpKey(octave_controls.second);
  }

  // Midi Learn Map
  if (config_properties.contains("midi_learn")) {
    MidiManager::midi_map midi_learn_map = midi_manager->getMidiLearnMap();

    Array<var>* midi_learn = config_properties["midi_learn"].getArray();
    var* midi_source = midi_learn->begin();

    for (; midi_source != midi_learn->end(); ++midi_source) {
      DynamicObject* source_object = midi_source->getDynamicObject();
      int source = source_object->getProperty("source");

      if (source_object->hasProperty("destinations")) {
        Array<var>* destinations = source_object->getProperty("destinations").getArray();
        var* midi_destination = destinations->begin();

        for (; midi_destination != destinations->end(); ++midi_destination) {
          DynamicObject* destination_object = midi_destination->getDynamicObject();

          String destination_name = destination_object->getProperty("destination").toString();
          mopo::mopo_float min_range = destination_object->getProperty("min_range");
          mopo::mopo_float max_range = destination_object->getProperty("max_range");

          midi_learn_map[source][destination_name.toStdString()] =
              MidiManager::midi_range(min_range, max_range);
        }
      }
    }
    midi_manager->setMidiLearnMap(midi_learn_map);
  }
}

bool LoadSave::isInstalled() {
  File factory_bank = getFactoryBankDirectory();
  return factory_bank.exists();
}

bool LoadSave::wasUpgraded() {
  var config_state = getConfigVar();
  DynamicObject* config_object = config_state.getDynamicObject();
  if (!config_state.isObject())
    return true;

  if (!config_object->hasProperty("synth_version"))
    return true;

  Array<File> patches;
  String extension = String("*.") + mopo::PATCH_EXTENSION;
  getBankDirectory().findChildFiles(patches, File::findFiles, true, extension);
  if (patches.size() == 0)
    return true;

  return compareVersionStrings(config_object->getProperty("synth_version"),
                               ProjectInfo::versionString) < 0;
}

bool LoadSave::shouldCheckForUpdates() {
  var config_state = getConfigVar();
  DynamicObject* config_object = config_state.getDynamicObject();
  if (!config_state.isObject())
    return true;

  if (!config_object->hasProperty("check_for_updates"))
    return true;

  return config_object->getProperty("check_for_updates");
}

bool LoadSave::shouldAnimateWidgets() {
  var config_state = getConfigVar();
  DynamicObject* config_object = config_state.getDynamicObject();
  if (!config_state.isObject())
    return true;

  if (!config_object->hasProperty("animate_widgets"))
    return true;

  return config_object->getProperty("animate_widgets");
}

std::wstring LoadSave::getComputerKeyboardLayout() {
  var config_state = getConfigVar();

  if (config_state.isVoid())
    return mopo::DEFAULT_KEYBOARD;

  DynamicObject* config_object = config_state.getDynamicObject();
  NamedValueSet config_properties = config_object->getProperties();

  if (config_properties.contains("keyboard_layout")) {
    DynamicObject* layout = config_properties["keyboard_layout"].getDynamicObject();

    if (layout->hasProperty("chromatic_layout"))
      return layout->getProperty("chromatic_layout").toString().toWideCharPointer();
  }

  return mopo::DEFAULT_KEYBOARD;
}

std::pair<wchar_t, wchar_t> LoadSave::getComputerKeyboardOctaveControls() {
  std::pair<wchar_t, wchar_t> octave_controls(mopo::DEFAULT_KEYBOARD_OCTAVE_DOWN,
                                              mopo::DEFAULT_KEYBOARD_OCTAVE_UP);
  var config_state = getConfigVar();
  if (config_state.isVoid())
    return octave_controls;

  DynamicObject* config_object = config_state.getDynamicObject();
  NamedValueSet config_properties = config_object->getProperties();

  if (config_properties.contains("keyboard_layout")) {
    DynamicObject* layout = config_properties["keyboard_layout"].getDynamicObject();
    octave_controls.first = layout->getProperty("octave_down").toString()[0];
    octave_controls.second = layout->getProperty("octave_up").toString()[0];
  }

  return octave_controls;
}

File LoadSave::getFactoryBankDirectory() {
  File patch_dir = File("");
#ifdef LINUX
  patch_dir = File(LINUX_FACTORY_PATCH_DIRECTORY);
#elif defined(__APPLE__)
  File data_dir = File::getSpecialLocation(File::commonApplicationDataDirectory);
  patch_dir = data_dir.getChildFile(String("Audio/Presets/") + "Helm");
#elif defined(_WIN32)
  File data_dir = File::getSpecialLocation(File::commonDocumentsDirectory);
  patch_dir = data_dir.getChildFile("Helm/Patches");
#endif

  return patch_dir;
}

File LoadSave::getBankDirectory() {
  if (!isInstalled())
    return File("../../../patches");

  File patch_dir = File("");
#ifdef LINUX
  patch_dir = File(LINUX_BANK_DIRECTORY);
#elif defined(__APPLE__)
  File data_dir = File::getSpecialLocation(File::userApplicationDataDirectory);
  patch_dir = data_dir.getChildFile(String("Audio/Presets/") + "Helm");
#elif defined(_WIN32)
  File documents_dir = File::getSpecialLocation(File::userDocumentsDirectory);
  File parent_dir = documents_dir.getChildFile("Helm");
  if (!parent_dir.exists())
    parent_dir.createDirectory();
  patch_dir = parent_dir.getChildFile("Patches");
#endif

  if (!patch_dir.exists())
    patch_dir.createDirectory();
  return patch_dir;
}

File LoadSave::getUserBankDirectory() {
  File bank_dir = getBankDirectory();
  File folder_dir = bank_dir.getChildFile(USER_BANK_NAME);

  if (!folder_dir.exists()) {
    folder_dir.createDirectory();
    for (String patch_folder : DEFAULT_USER_FOLDERS)
      folder_dir.getChildFile(patch_folder).createDirectory();
  }
  return folder_dir;
}

int LoadSave::compareVersionStrings(String a, String b) {
  a.trim();
  b.trim();

  if (a.isEmpty() && b.isEmpty())
    return 0;

  String major_version_a = a.upToFirstOccurrenceOf(".", false, true);
  String major_version_b = b.upToFirstOccurrenceOf(".", false, true);

  if (!major_version_a.containsOnly("0123456789"))
    major_version_a = "0";
  if (!major_version_b.containsOnly("0123456789"))
    major_version_b = "0";

  int major_value_a = major_version_a.getIntValue();
  int major_value_b = major_version_b.getIntValue();

  if (major_value_a > major_value_b)
    return 1;
  else if (major_value_a < major_value_b)
    return -1;
  return compareVersionStrings(a.fromFirstOccurrenceOf(".", false, true),
                               b.fromFirstOccurrenceOf(".", false, true));
}

int LoadSave::getNumPatches() {
  File bank_directory;
  bank_directory = getBankDirectory();

  Array<File> patches;
  bank_directory.findChildFiles(patches, File::findFiles, true,
                                String("*.") + mopo::PATCH_EXTENSION);
  return patches.size();
}

File LoadSave::getPatchFile(int bank_index, int folder_index, int patch_index) {
  static const FileSorterAscending file_sorter;

  File bank_directory = getBankDirectory();
  Array<File> banks;
  bank_directory.findChildFiles(banks, File::findDirectories, false);
  banks.sort(file_sorter);

  if (banks.size() == 0)
    return File();

  if (bank_index >= 0) {
    File bank = banks[std::min(bank_index, banks.size() - 1)];
    banks.clear();
    banks.add(bank);
  }

  Array<File> folders;
  for (File bank : banks) {
    Array<File> bank_folders;
    bank.findChildFiles(bank_folders, File::findDirectories, false);
    bank_folders.sort(file_sorter);
    folders.addArray(bank_folders);
  }

  if (folders.size() == 0)
    return File();

  if (folder_index >= 0) {
    File folder = folders[std::min(folder_index, folders.size() - 1)];
    folders.clear();
    folders.add(folder);
  }

  Array<File> patches;
  for (File folder : folders) {
    Array<File> folder_patches;
    folder.findChildFiles(folder_patches, File::findFiles, false,
                          String("*.") + mopo::PATCH_EXTENSION);
    folder_patches.sort(file_sorter);
    patches.addArray(folder_patches);
  }

  if (patches.size() == 0 || patch_index < 0)
    return File();

  return patches[std::min(patch_index, patches.size() - 1)];
}

File LoadSave::loadPatch(int bank_index, int folder_index, int patch_index,
                         mopo::HelmEngine* synth, std::map<std::string, String>& save_info) {
  File patch = getPatchFile(bank_index, folder_index, patch_index);

  var parsed_json_state;
  if (JSON::parse(patch.loadFileAsString(), parsed_json_state).wasOk())
    varToState(synth, save_info, parsed_json_state);

  return patch;
}
