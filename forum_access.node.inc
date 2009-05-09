<?php
// $Id$

/**
 * @file forum_access.node.inc
 *
 * Include file for forum_access.module, containing (sub-)page handling
 * (form_alter) code for the node and comment forms as well as code
 * for temporarily assigning the Forum Moderator role and managing
 * the resulting rights.
 */

/**
 * Rewrite the taxonomy item on the node form.
 */
function _forum_access_node_form(&$form, &$form_state) {
  global $user;
  $vid = variable_get('forum_nav_vocabulary', '');

  if (!isset($form['taxonomy'][$vid]['#options'])) {
    return;
  }

  // Node administrators are all powerful and do NOT get their forms rewritten here.
  if (user_access('administer nodes') && empty($user->_forum_access_moderator)) {
    return;
  }

  $roles = _forum_access_get_roles($user);
  $result = db_query("SELECT tid FROM {forum_access} WHERE rid IN (%s) AND grant_create = 1", $roles);
  while ($obj = db_fetch_object($result)) {
    $tids[$obj->tid] = $obj->tid;
  }

  // Also get all forums they happen to be able to moderate.
  $result = db_query("SELECT a.name AS tid FROM {acl} a INNER JOIN {acl_user} u ON a.acl_id = u.acl_id WHERE a.module = 'forum_access' AND u.uid = %d", $user->uid);
  while ($obj = db_fetch_object($result)) {
    $tids[$obj->tid] = $obj->tid;
  }

  // Ensure the forum they're trying to post to directly is allowed, otherwise
  // there will be much confusion.
  $forum_tid = arg(3);
  if (isset($forum_tid) && is_numeric($forum_tid) && !isset($tids[$forum_tid])) {
    drupal_access_denied();
    module_invoke_all('exit');
    exit;
  }

  foreach ($form['taxonomy'][$vid]['#options'] as $tid => $name) {
    if (!is_numeric($tid)) {
      $options[$tid] = $name;
    }
    elseif (is_object($name)) {
      foreach ($name->option as $sub_tid => $sub_name) {
        if (!empty($tids[$sub_tid])) {
          $options[$tid]->option[$sub_tid] = $sub_name;
        }
      }
    }
    elseif ($tids[$tid]) {
      $options[$tid] = $name;
    }
  }

  if ($options) {
    $form['taxonomy'][$vid]['#options'] = $options;
  }
  else {
    unset($form['taxonomy'][$vid]);
  }

  // Apply modifications for Moderators (by role or uid).
  if (!empty($user->_forum_access_moderator)) {
    // We gave this user the 'administer nodes' permission, which he doesn't
    // normally have. Remove controls that should be reserved to true node
    // administrators.
    $allowed_elements = variable_get('forum_access_allowed_node_edit_elements', array('nid', 'vid', 'uid', 'created', 'type', 'changed', 'title', 'shadow', 'body_field', 'revision_information', 'form_build_id', 'form_token', 'form_id', 'comment_settings', 'taxonomy'));
    $allowed_options = variable_get('forum_access_allowed_node_edit_options', array('status', 'sticky', 'subscriptions_notify'));
    foreach (element_children($form) as $key) {
      switch ($key) {
        case 'options':
          foreach (element_children($form[$key]) as $key2) {
            if (array_search($key2, $allowed_options) === FALSE) {
              $form[$key][$key2]['#access'] = FALSE;
            }
          }
          break;
        case 'buttons':
          $tid = $form['taxonomy'][$vid]['#default_value'][0];
          if (!forum_access_access($tid, 'update')) {
            $form['buttons']['submit']['#access'] = FALSE;
            $form['buttons']['preview']['#access'] = FALSE;
          }
          if (!forum_access_access($tid, 'delete')) {
            $form['buttons']['delete']['#access'] = FALSE;
          }
          break;
        default:
          if (array_search($key, $allowed_elements) === FALSE) {
            $form[$key]['#access'] = FALSE;
          }
      }
    }
  }
}

/**
 * Remove the in-line 'Post new comment' form, if the user does not have the
 * 'create' permission (see below).
 * (This needs forum_access_preprocess_box() to clean up afterwards.)
 *
 * Also, deny access if the user tries to enter a comment URL directly,
 * and sanitize the Administration options for users with Edit grants.
 */
function _forum_access_comment_form(&$form, &$form_state) {
  global $user;
  if ($user->uid != 1 && isset($form['nid']['#value'])) {
    $node = node_load($form['nid']['#value']);
    if ($tid = _forum_access_get_tid($node)) {
      if (!forum_access_access($tid, 'create')) {
        switch (arg(0))
        {
          case 'node':
            $form = NULL;  // remove the in-line comment form
            break;
          case 'comment':
            drupal_access_denied();
            module_invoke_all('exit');
            exit;
        }
      }
      else {
        if (isset($form['admin']) && !empty($user->_forum_access_moderator)) {
          foreach (element_children($form['admin']) as $key) {
            if ($key != 'status') {
              $form['admin'][$key]['#access'] = FALSE;
            }
          }
        }
      }
    }
  }
}

/*
 * Give the user the 'administer nodes' and 'administer comments' permissions for this request.
 */
function _forum_access_enable_moderator() {
  global $user;
  $rid = _forum_access_get_moderator_rid();
  $user->roles[$rid] = '(forum_access temporary)';
  user_access('', NULL, TRUE);  // clear the permissions cache to activate the new role
}

/*
 * Remove the moderator permissions.
 */
function _forum_access_disable_moderator() {
  global $user;
  $rid = _forum_access_get_moderator_rid();
  unset($user->roles[$rid]);
  user_access('', NULL, TRUE);  // clear the permissions cache to revert to normal
}

/*
 * Retrieve the rid of the Forum Moderator role; if the role does not exist,
 * then create it.
 */
function _forum_access_get_moderator_rid($verbose = FALSE)
{
  $rid = variable_get('forum_access_moderator_rid', NULL);
  if ($rid !== NULL) {
    if (db_result(db_query("SELECT COUNT(rid) FROM {role} WHERE rid = %d", $rid)) == 1) {
      return $rid;
    }
  }
  module_load_include('admin.inc', 'forum_access');
  return _forum_access_create_moderator_rid($verbose);
}

/*
 * Return the roles for forum_access_node_access_explain().
 */
function _forum_access_get_all_roles() { 
  $roles = user_roles();
  $moderator_rid = variable_get('forum_access_moderator_rid', NULL);
  if (isset($roles[$moderator_rid])) {
    $roles[$moderator_rid] .= ' '. t('(!Forum_Access temporary role, does not need any grants.)', array('!Forum_Access' => 'Forum Access'));
  }
  return $roles;
}

/**
 * Recreate comment links (they've already been themed), and
 * remove those that aren't accessible to the user.
 */
function _forum_access_preprocess_comment(&$variables) {
  global $user;
  if (!empty($user->_forum_access_moderator)) {
    _forum_access_enable_moderator();  // this allows us to retrieve the comment links (without setting precedent!)
  }

  $tid = $variables['node']->tid;
  $links = module_invoke_all('link', 'comment', $variables['comment'], 0);
  if (isset($links['comment_reply']) && (!preg_match('#<li class="[^"]*comment_reply[^"]*".*</li>#U', $variables['links']) || !forum_access_access($tid, 'create'))) {
    unset($links['comment_reply']);
  }
  if (isset($links['comment_edit']) && !forum_access_access($tid, 'update')) {
    unset($links['comment_edit']);
  }
  if (isset($links['comment_delete']) && !forum_access_access($tid, 'delete')) {
    unset($links['comment_delete']);
  }
  foreach(array_keys($links) as $link) {
    if (!in_array($link, array('comment_reply', 'comment_edit', 'comment_delete')) && !preg_match('#<li class="[^"]*'. $link .'[^"]*".*</li>#U', $variables['links'])) {
      unset($links[$link]);   // eliminate possible additional unknown links that came in for 'administer_comments'
    }
  }
  if (empty($links)) {
    $links['comment_forbidden'] = array(
      'title' => theme('comment_post_forbidden', $variables['node']),
      'html'  => TRUE,
    );
  }
  $variables['links'] = theme('links', $links);
  
  if (!empty($user->_forum_access_moderator) && arg(0) == 'node' && arg(2) == NULL) {
    // Remove the permissions again to avoid confusing devel_node_access.module.
    _forum_access_disable_moderator();
  }
}