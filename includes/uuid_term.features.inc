<?php
/**
 * @file
 * Features hooks for the uuid_term features component.
 */

/**
 * Implements hook_features_export_options().
 */
function uuid_term_features_export_options() {
  $options = array();

  $query = 'SELECT t.tid, t.name, v.name AS vname, t.uuid
    FROM {taxonomy_term_data} t
    INNER JOIN {taxonomy_vocabulary} v ON t.vid = v.vid
    ORDER BY v.name ASC, t.name ASC';
  $results = db_query($query);
  foreach ($results as $term) {
    $options[$term->uuid] = $term->vname . ' - ' . $term->name;
  }

  return $options;
}

/**
 * Implements hook_features_export().
 */
function uuid_term_features_export($data, &$export, $module_name = '') {
  $export['dependencies']['taxonomy'] = 'taxonomy';
  $export['dependencies']['uuid'] = 'uuid';
  $export['dependencies']['uuid_features'] = 'uuid_features';

  foreach ($data as $uuid) {
    uuid_term_features_get_dependencies($export, $uuid);
  }
  return array();
}

/**
 * Adds terms and its dependencies to the export.
 *
 * Parents and term references are handled recrusively, other references are not
 * yet supported.
 */
function uuid_term_features_get_dependencies(&$export, $uuid) {
  $terms = entity_uuid_load('taxonomy_term', array($uuid));
  if (count($terms)) {
    $term = reset($terms);
    $export['features']['uuid_term'][$uuid] = $uuid;
    $export['features']['taxonomy'][$term->vocabulary_machine_name] = $term->vocabulary_machine_name;
    // Recursively add all parents and the references of the parents.
    foreach (taxonomy_get_parents($term->tid) as $parent) {
      if (!in_array($parent->uuid, $export['features']['uuid_term'])) {
        uuid_term_features_get_dependencies($export, $parent->uuid);
      }
    }
    // Get term references.
    $instances = field_info_instances('taxonomy_term', $term->vocabulary_machine_name);
    foreach ($instances as $field_name => $instance) {
      $field = field_info_field($field_name);
      if ($field['type'] == 'taxonomy_term_reference') {
        if (isset($term->$field_name)) {
          foreach ($term->$field_name as $lang => $values) {
            foreach ($values as $value) {
              // $value['tid'] already contains the UUID.
              if (!in_array($value['tid'], $export['features']['uuid_term'])) {
                uuid_term_features_get_dependencies($export, $value['tid']);
              }
            }
          }
        }
      }
    }
  }
}

/**
 * Implements hook_features_export_render().
 */
function uuid_term_features_export_render($module = 'foo', $data) {
  $translatables = $code = array();

  $code[] = '  $terms = array();';
  $code[] = '';
  foreach ($data as $uuid) {
    // @todo reset = TRUE as otherwise references (parent, fields) were destroyed.
    $terms = entity_uuid_load('taxonomy_term', array($uuid), array(), TRUE);
    if (!count($terms)) {
      continue;
    }

    // Export the parent uuids.
    foreach ($terms as $term) {
      if($parents = taxonomy_get_parents($term->tid)) {
        foreach ($parents as $parent) {
          $term->parent[] = $parent->uuid;
        }
      }
    }

    $export = reset($terms);

    // Do not export ids.
    unset($export->vid);
    unset($export->tid);
    // No need to export the rdf mapping.
    unset($export->rdf_mapping);
    uuid_term_features_file_field_export($export);
    $code[] = '  $terms[] = ' . features_var_export($export, '  ') . ';';
  }

  if (!empty($translatables)) {
    $code[] = features_translatables_export($translatables, '  ');
  }
  // Configuration settings need to be exported along with terms to
  // avoid diffs between the default and normal code returned by
  // this function.
  $code[] = '  variable_set(\'uuid_features_file_types\', ' . features_var_export(variable_get('uuid_features_file_types', array())) . ');';
  $code[] = '  variable_set(\'uuid_features_file_mode\', ' . features_var_export(variable_get('uuid_features_file_mode', 'inline')) . ');';
  $code[] = '  variable_set(\'uuid_features_file_assets_path\', ' . features_var_export(variable_get('uuid_features_file_assets_path', '')) . ');';
  $code[] = '  variable_set(\'uuid_features_file_supported_fields\', ' . features_var_export(variable_get('uuid_features_file_supported_fields', 'file, image')) . ');';
  $code[] = '  return $terms;';
  $code = implode("\n", $code);
  return array('uuid_features_default_terms' => $code);
}

/**
 * Implements hook_features_revert().
 */
function uuid_term_features_revert($module) {
  uuid_term_features_rebuild($module);
}

/**
 * Implements hook_features_rebuild().
 * Rebuilds terms based on UUID from code defaults.
 */
function uuid_term_features_rebuild($module) {
  // Import the vocabularies first.
  taxonomy_features_rebuild($module);
  field_features_rebuild($module);

  $terms = module_invoke($module, 'uuid_features_default_terms');
  if (!empty($terms)) {
    // Verify that term objects is saved before any references are resolved.
    foreach ($terms as $data) {
      $existing = entity_get_id_by_uuid('taxonomy_term', array($data['uuid']));
      if (!count($existing)) {
        $voc = taxonomy_vocabulary_machine_name_load($data['vocabulary_machine_name']);
        // Only save the term, if the corresponding vocabulary already exisits.
        if ($voc) {
          $term = new stdClass;
          $term->uuid = $data['uuid'];
          $term->vid = $voc->vid;
          $term->name = $data['name'];
          taxonomy_term_save($term);
        }
      }
    }
    // Save all other data and resolve references.
    foreach ($terms as $data) {
      $term = (object) $data;
      $voc = taxonomy_vocabulary_machine_name_load($term->vocabulary_machine_name);
      if ($voc) {
        $term->vid = $voc->vid;
        uuid_term_features_file_field_import($term, $voc);
        entity_uuid_save('taxonomy_term', $term);
      }
    }
  }
}

/**
 * Handle exporting file fields.
 */
function uuid_term_features_file_field_export(&$term) {
  $vocabularies = array_filter(variable_get('uuid_features_file_types', array()));
  if (in_array($term->vocabulary_machine_name, $vocabularies)) {
    $orig_assets_path = $assets_path = variable_get('uuid_features_file_assets_path', '');
    $export_mode = variable_get('uuid_features_file_mode', 'inline');

    switch ($export_mode) {
      case 'local':
        $export_var = 'uuid_features_file_path';
        break;
      case 'remote':
        $export_var = 'uuid_features_file_url';
        break;
      default:
      case 'inline':
        $export_var = 'uuid_features_file_data';
        break;
    }
    // If files are supposed to be copied to the assets path.
    if ($export_mode == 'local' && $assets_path) {
      // Ensure the assets path is created
      if ((!is_dir($assets_path) && mkdir($assets_path, 0777, TRUE) == FALSE)
        || !is_writable($assets_path)
      ) {
        // Try creating a public path if the local path isn't writeable.
        // This is a kludgy solution to allow writing file assets to places
        // such as the profiles/myprofile directory, which isn't supposed to
        // be writeable
        $new_assets_path = 'public://' . $assets_path;
        if (!is_dir($new_assets_path) && mkdir($new_assets_path, 0777, TRUE) == FALSE) {
          drupal_set_message(t("Could not create assets path! '!path'", array('!path' => $assets_path)), 'error');
          // Don't continue if the assets path is not ready
          return;
        }
        $assets_path = $new_assets_path;
      }
    }

    // get all fields from this vocabulary
    $fields = field_info_instances('taxonomy_term', $term->vocabulary_machine_name);
    foreach ($fields as $field_instance) {
      // load field infos to check the type
      $field = &$term->{$field_instance['field_name']};
      $info = field_info_field($field_instance['field_name']);

      $supported_fields = array_map('trim', explode(',', variable_get('uuid_features_file_supported_fields', 'file, image')));

      // check if this field should implement file import/export system
      if (in_array($info['type'], $supported_fields)) {

        // we need to loop into each language because i18n translation can build
        // fields with different language than the node one.
        foreach($field as $language => $files) {
          if (is_array($files)) {
            foreach($files as $i => $file) {

              // convert file to array to stay into the default uuid_features_file format
              $file = (object) $file;

              // Check the file
              if (!isset($file->uri) || !is_file($file->uri)) {
                drupal_set_message(t("File field found on term, but file doesn't exist on disk? '!path'", array('!path' => $file->uri)), 'error');
                continue;
              }

              if ($export_mode == 'local') {
                if ($assets_path) {
                  // The writeable path may be different from the path that gets saved
                  // during the feature export to handle the public path/local path
                  // dilemma mentioned above.
                  $writeable_export_data = $assets_path . '/' . basename($file->uri);
                  $export_data = $orig_assets_path . '/' . basename($file->uri);
                  if (!copy($file->uri, $writeable_export_data)) {
                    drupal_set_message(t("Export file error, could not copy '%filepath' to '%exportpath'.", array('%filepath' => $file->uri, '%exportpath' => $writeable_export_data)), 'error');
                    return FALSE;
                  }
                }
                else {
                  $export_data = $file->uri;
                }
              }
              // Remote export mode
              elseif ($export_mode == 'remote') {
                $export_data = url($file->uri, array('absolute' => TRUE));
              }
              // Default is 'inline' export mode
              else {
                $export_data = base64_encode(file_get_contents($file->uri));
              }

              // build the field again, and remove fid to be sure that imported node
              // will rebuild the file again, or keep an existing one with a different fid
              $field[$language][$i]['fid'] = NULL;
              $field[$language][$i]['timestamp'] = NULL;
              $field[$language][$i][$export_var] = $export_data;
            }
          }
        }
      }
    }
  }
}

/**
 * Handle importing file fields.
 */
function uuid_term_features_file_field_import(&$term, $voc) {
  // Get all fields from this vocabulary.
  $fields = field_info_instances('taxonomy_term', $term->vocabulary_machine_name);

  foreach($fields as $field_instance) {
    // Load field info to check the type.
    $field = &$term->{$field_instance['field_name']};
    $info = field_info_field($field_instance['field_name']);

    $supported_fields = array_map('trim', explode(',', variable_get('uuid_features_file_supported_fields', 'file, image')));

    // Check if this field should implement file import/export system.
    if (in_array($info['type'], $supported_fields)) {

      // We need to loop into each language because i18n translation can build
      // fields with different language than the term one.
      foreach($field as $language => $files) {
        if (is_array($files)) {
          foreach($files as $i => $file) {

            // Convert file to array to stay into the default uuid_features_file format.
            $file = (object)$file;

            $result = _uuid_features_file_field_import_file($file);
            // The file was saved successfully, update the file field (by reference).
            if ($result == TRUE && isset($file->fid)) {
              $field[$language][$i] = (array)$file;
            }

          }
        }
      }
    }
  }
}
