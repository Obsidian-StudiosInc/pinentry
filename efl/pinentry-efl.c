/* pinentry-efl.c
   Copyright (C) 2017 Obsidian-Studios, Inc.
     Author William L. Thomson Jr. <wlt@o-sinc.com>
   Copyright (C) 2017 Mike Blumenkrantz <zmike@osg.samsung.com>

   Based on pinentry-gtk2.c
   Copyright (C) 1999 Robert Bihlmeyer <robbe@orcus.priv.at>
   Copyright (C) 2001, 2002, 2007, 2015 g10 Code GmbH
   Copyright (C) 2004 by Albrecht Dreß <albrecht.dress@arcor.de>
 
   pinentry-efl is a pinentry application for the EFL widget set.
   It tries to follow the Gnome Human Interface Guide as close as
   possible.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <Elementary.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gpg-error.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
#include "getopt.h"
#endif				/* HAVE_GETOPT_H */

#include "pinentry.h"

#ifdef FALLBACK_CURSES
#include "pinentry-curses.h"
#endif


#define PGMNAME "pinentry-efl"

#ifndef VERSION
#  define VERSION
#endif

static pinentry_t pinentry;
static int grab_failed;
static int passphrase_ok;
typedef enum { CONFIRM_CANCEL, CONFIRM_OK, CONFIRM_NOTOK } confirm_value_t;
static confirm_value_t confirm_value;
static Eina_Bool got_input;
static Ecore_Timer *timer;
static Evas_Object *win, *entry, *error_label, *repeat_entry, *qualitybar;
static int confirm_mode;

const static int height = 190;
const static int width = 400;
const static int padding = 5;
const static int button_height = 30;
const static int button_width = 100;

static void
quit ()
{
  evas_object_del(win);
  elm_exit();
}

static void
delete_event (void *data, Evas_Object *obj, void *event)
{
  (void)data;
  (void)obj;
  (void)event;

  pinentry->close_button = 1;
  quit ();
}

static void
on_cancel (void *data, Evas_Object *obj, void *event)
{
  (void)data;
  (void)obj;
  (void)event;

  confirm_value = CONFIRM_CANCEL;
  quit();
}
static void
changed_text_handler (Evas_Object *obj)
{
  char textbuf[50];
  const char *s;
  int length;
  int percent;

  got_input = EINA_TRUE;

  if (pinentry->repeat_passphrase && repeat_entry)
    {
      elm_object_text_set (repeat_entry, "");
      elm_object_text_set (error_label, "");
    }

  if (!qualitybar || !pinentry->quality_bar)
    return;

  s = elm_object_text_get (obj);
  if (!s)
    s = "";
  length = strlen (s);
  percent = length? pinentry_inq_quality (pinentry, s, length) : 0;
  if (!length)
    {
      strcpy(textbuf, " ");
    }
  else if (percent < 0)
    {
      snprintf (textbuf, sizeof textbuf, "(%d%%)", -percent);
      textbuf[sizeof textbuf -1] = 0;
      evas_object_color_set(qualitybar, 255, 0, 0, 255);
      percent = -percent;
    }
  else
    {
      snprintf (textbuf, sizeof textbuf, "%d%%", percent);
      textbuf[sizeof textbuf -1] = 0;
      evas_object_color_set(qualitybar, 0, 255, 0, 255);
    }

  elm_obj_progressbar_part_value_set (qualitybar, (double)percent/100.0);
  elm_object_text_set (qualitybar, textbuf);
}

static void
on_check (void *data, Evas_Object *obj, void *event)
{
    if(elm_check_selected_get(obj))
        elm_entry_password_set(entry, EINA_FALSE);
    else
        elm_entry_password_set(entry, EINA_TRUE);
}

static void
on_click (void *data, Evas_Object *obj, void *event)
{
  confirm_value = CONFIRM_OK;
  if (confirm_mode)
    {
      confirm_value = (confirm_value_t) data;
      elm_exit ();

      return;
    }
  if (data)
    {
      const char *s, *s2;

      s = elm_object_text_get (entry);
      if (!s)
	s = "";

      if (pinentry->repeat_passphrase && repeat_entry)
        {
          s2 = elm_object_text_get (repeat_entry);
          if (!s2)
            s2 = "";
          if (strcmp (s, s2))
            {
              elm_object_text_set(error_label,
                                  pinentry->repeat_error_string?
                                  pinentry->repeat_error_string:
                                  "not correctly repeated");
              elm_object_focus_set(entry,EINA_TRUE);
              return;
            }
          pinentry->repeat_okay = 1;
        }

      passphrase_ok = 1;
      pinentry_setbufferlen (pinentry, strlen (s) + 1);
      if (pinentry->pin)
	strcpy (pinentry->pin, s);
    }
  quit();
}

static Eina_Bool
timeout_cb (const void * data)
{
  printf("timeout_cb called\n");
  pinentry_t pe = (pinentry_t)data;
  if (!got_input)
    {
      quit();
      if (pe)
        pe->specific_err = gpg_error (GPG_ERR_TIMEOUT);
    }

  timer = NULL;
  return ECORE_CALLBACK_DONE;
}

static Evas_Object *
create_window (pinentry_t ctx)
{
  char *txt;
  Evas_Object *hbox, *table, *obj, *ok, *cancel;
  int row = 0;

  elm_policy_set(ELM_POLICY_QUIT, ELM_POLICY_QUIT_LAST_WINDOW_CLOSED);

  win = elm_win_util_standard_add("pinentry","enter pin");
  elm_win_autodel_set(win, EINA_TRUE);
  evas_object_smart_callback_add(win, "delete,request", delete_event, NULL);

  table = elm_table_add(win);
  elm_obj_table_padding_set(table, padding, padding);
  evas_object_size_hint_min_set(table, ELM_SCALE_SIZE(width), 1);
  evas_object_show(table);

  if (pinentry->title)
    {
      txt = elm_entry_utf8_to_markup(pinentry->title);
      elm_win_title_set ( win, txt );
      free (txt);
    }

  if (pinentry->description)
    {
      /* Description Label */
      obj = elm_label_add(table);
      elm_label_line_wrap_set (obj, ELM_WRAP_WORD);
      txt = elm_entry_utf8_to_markup(pinentry->description);
      elm_object_text_set(obj,txt);
      free (txt);
      evas_object_size_hint_weight_set(obj, EVAS_HINT_EXPAND, 0);
      evas_object_size_hint_align_set(obj, EVAS_HINT_FILL, 0);
      elm_table_pack(table, obj, 1, row, 5, 1);
      evas_object_show(obj);
      row++;
    }
  if (!confirm_mode && (pinentry->error || pinentry->repeat_passphrase))
    {
    /* Error Label */
    if (pinentry->error)
        txt = elm_entry_utf8_to_markup (pinentry->error);
      else
        txt = "";
      obj = elm_label_add(table);
      evas_object_color_set(obj, 255, 0, 0, 255);
      elm_object_text_set(obj,txt);
      elm_object_style_set(obj,"slide_bounce");
      elm_label_slide_duration_set(obj, 10);
      elm_label_slide_mode_set(obj, ELM_LABEL_SLIDE_MODE_ALWAYS);
      elm_label_slide_go(obj);
      evas_object_size_hint_weight_set(obj, EVAS_HINT_EXPAND, 0);
      evas_object_size_hint_align_set(obj, EVAS_HINT_FILL, 0);
      elm_table_pack(table, obj, 1, row, 5, 1);
      evas_object_show(obj);
      if (pinentry->error)
        free (txt);
      row++;
    }

  if (!confirm_mode)
    {

      /* Entry Label */
      obj = elm_label_add(table);
      elm_object_text_set(obj,"Passphrase:");
      evas_object_size_hint_weight_set(obj, 0, 0);
      evas_object_size_hint_align_set(obj, 1, 0);
      elm_table_pack(table, obj, 1, row, 1, 1);
      evas_object_show(obj);

      entry = elm_entry_add(table);
      elm_entry_scrollable_set(entry, EINA_TRUE);
      elm_scroller_policy_set(entry, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
      elm_entry_password_set(entry, EINA_TRUE);
      elm_entry_single_line_set(entry, EINA_TRUE);
      evas_object_size_hint_weight_set(entry, 0, 0);
      evas_object_size_hint_align_set(entry, EVAS_HINT_FILL, 0);
      elm_table_pack(table, entry, 2, row, 4, 1);
      evas_object_smart_callback_add(entry, "changed", changed_text_handler, win);
      evas_object_show(entry);
      row++;

      /* Check box */
      obj = elm_check_add(table);
      evas_object_size_hint_align_set(obj, 1, 0);
      elm_table_pack(table, obj, 1, row, 1, 1);
      evas_object_smart_callback_add(obj, "changed", on_check, win);
      evas_object_show(obj);

      /* Check Label */
      obj = elm_label_add(table);
      elm_object_text_set(obj,"Make passphrase visible");
      evas_object_size_hint_weight_set(obj, 0, 0);
      evas_object_size_hint_align_set(obj, 0, 0);
      elm_table_pack(table, obj, 2, row, 4, 1);
      evas_object_show(obj);
      row++;

      if (pinentry->quality_bar)
	{
          txt = elm_entry_utf8_to_markup (pinentry->quality_bar);
/*
	  qualitybar_label = elm_label_add(table);
          elm_object_text_set(qualitybar_label,txt);
          evas_object_size_hint_weight_set(qualitybar_label, 0, 0);
          evas_object_size_hint_align_set(qualitybar_label, 0, 0);
          elm_table_pack(table, qualitybar_label, 0, row, 1, 1);
*/

	  qualitybar = elm_progressbar_add(table);

          elm_progressbar_unit_format_set(qualitybar, "%1.2f%%");
/*
          elm_object_text_set(qualitybar,txt);
          elm_progressbar_pulse_set(qualitybar, EINA_TRUE);
          elm_progressbar_pulse(qualitybar, EINA_TRUE
	  elm_obj_progressbar_part_value_set (qualitybar, 0.0);
*/
          if (pinentry->quality_bar_tt)
	    {
	      elm_object_tooltip_text_set (qualitybar,
					   pinentry->quality_bar_tt);
	    }
/*
          evas_object_size_hint_weight_set(qualitybar, 0, 0);
          evas_object_size_hint_align_set(qualitybar, EVAS_HINT_FILL, 0);
*/
          elm_table_pack(table, qualitybar, 1, row, 4, 1);
          free (txt);
          row++;
	}

      if (pinentry->repeat_passphrase)
        {
	  txt = elm_entry_utf8_to_markup (pinentry->repeat_passphrase);
          repeat_entry = elm_entry_add(table);
          elm_entry_scrollable_set(repeat_entry, EINA_TRUE);
          elm_scroller_policy_set(repeat_entry, ELM_SCROLLER_POLICY_OFF, ELM_SCROLLER_POLICY_OFF);
          elm_entry_password_set(repeat_entry, EINA_TRUE);
          elm_entry_single_line_set(repeat_entry, EINA_TRUE);
          elm_object_text_set(repeat_entry,txt);
          evas_object_size_hint_weight_set(repeat_entry, 0, 0);
          evas_object_size_hint_align_set(repeat_entry, EVAS_HINT_FILL, 0);
          elm_table_pack(table, repeat_entry, 2, row, 4, 1);
          evas_object_show(repeat_entry);
	  free (txt);
          row++;
        }
  }
  
  if (!pinentry->one_button)
    {
      cancel = elm_button_add(table);
      if (pinentry->cancel)
        {
          txt = elm_entry_utf8_to_markup (pinentry->cancel);
          elm_object_text_set(cancel,txt);
          free (txt);
        }
      else if (pinentry->default_cancel)
        {
          Evas_Object *ic;

          txt = elm_entry_utf8_to_markup (pinentry->default_cancel);
          elm_object_text_set(cancel,txt);
          free (txt);
/*
          ic = elm_icon_add(cancel);
          elm_image_file_set(ic, "icon.png", NULL); //STOCK_CANCEL
          elm_object_part_content_set(cancel, "icon", ic);
*/
        }
      else
        elm_object_text_set(cancel, " Cancel  "); //STOCK_CANCEL
      evas_object_size_hint_align_set(cancel, 0, 0);
      elm_table_pack(table, cancel, 4, row, 1, 1);
      evas_object_smart_callback_add(cancel, "clicked", on_cancel, win);
      evas_object_show(cancel);
    }
   
  ok = elm_button_add(table);
  if (pinentry->ok)
    {
      txt = elm_entry_utf8_to_markup (pinentry->ok);
      elm_object_text_set(ok,txt);
      free (txt);
    }
  else if (pinentry->default_ok)
    {
      Evas_Object *ic;

      txt = elm_entry_utf8_to_markup (pinentry->ok);
      elm_object_text_set(ok,txt);
      free (txt);
/*
      ic = elm_icon_add(ok);
      elm_image_file_set(ic, "icon.png", NULL);//STOCK_OK
      elm_object_part_content_set(ok, "icon", ic);
*/
    }
  else
    elm_object_text_set(ok,"    OK     "); //STOCK_OK
  evas_object_size_hint_align_set(ok, 0, 0);
  elm_table_pack(table, ok, 5, row, 1, 1);
  evas_object_smart_callback_add(ok, "clicked", on_click, win);
  evas_object_show(ok);

  obj = elm_icon_add (win);
  evas_object_size_hint_aspect_set (obj, EVAS_ASPECT_CONTROL_BOTH, 1, 1);
  /* FIXME: need some sort of key icon... */
  if (elm_icon_standard_set (obj, "system-lock-screen"))
    {
      evas_object_size_hint_min_set(obj, ELM_SCALE_SIZE(width/5), ELM_SCALE_SIZE(width/5));
      evas_object_size_hint_weight_set(obj, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
      evas_object_size_hint_align_set(obj, EVAS_HINT_FILL, 0.5);
      elm_table_pack(table, obj, 0, 0, 1, row);
      evas_object_show (obj);
    }
  else
    {
       evas_object_del(obj);
/*
       noimage = EINA_TRUE;
*/
    }

  elm_win_resize_object_add(win,table);
  elm_win_center(win,EINA_TRUE,EINA_TRUE);
  elm_win_raise(win);
  evas_object_show(win);

  if (pinentry->timeout > 0)
    timer = ecore_timer_add (pinentry->timeout, timeout_cb, pinentry);

  return win;
}

static int
elm_cmd_handler (pinentry_t pe)
{
  Evas_Object *w;
  int want_pass = !!pe->pin;

  got_input = EINA_FALSE;
  pinentry = pe;
  confirm_value = CONFIRM_CANCEL;
  passphrase_ok = 0;
  confirm_mode = want_pass ? 0 : 1;

  w = create_window (pe);
  elm_run();

/*
  if (timer)
    {
      ecore_timer_del (timer);
      timer = NULL;
    }
*/

  if (confirm_value == CONFIRM_CANCEL || grab_failed)
    pe->canceled = 1;

  pinentry = NULL;
  if (want_pass)
    {
      if (passphrase_ok && pe->pin)
	return strlen (pe->pin);
      else
	return -1;
    }
  else
    return (confirm_value == CONFIRM_OK) ? 1 : 0;
}

pinentry_cmd_handler_t pinentry_cmd_handler = elm_cmd_handler;

int
main (int argc, char *argv[])
{
  pinentry_init (PGMNAME);

  elm_init (argc, argv);

  pinentry_parse_opts (argc, argv);

  if (pinentry_loop ())
    return 1;

  return 0;
}
