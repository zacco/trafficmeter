/* 
  TrafficMeter
  Author: Lajos Zaccomer
  Contact: lajos at zaccomer dot org

  Version history
 
   0.0.1    Initial version
            * basic counter
            * interface selection
            * two-level warning with colours
            * configurable display unit
            * saving settings in rc file

   0.0.2    Log file with timestamps added

   0.0.3    Windows build corrections for MSVC9
            Interface combo sensitivity change at start/stop

   0.0.4    Tray icon view, hide when minimized based on:
              http://www.codeproject.com/KB/cross-platform/GTKTrayIcon.aspx
            Text markup replaced with info bar
*/


#define VERSION "0.0.4"

#ifdef WIN32
#define _WINSOCKAPI_
#include <windows.h>
/* NOTE: The windows version of chdir() does not accept NULL input! */
#include <direct.h>
#define chdir(path) ((path) != NULL ? _chdir(path) : -1)
#else /* !WIN32 */
#include <unistd.h>
#endif /* !WIN32 */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <time.h>
#include <pcap.h>
#include <gtk/gtk.h>

#define SOFT_LIMIT 1800000000
#define HARD_LIMIT 2000000000
#define RCNAME  ".trafficmeterrc"
#define LOGNAME ".trafficmeterlog"
#define ENTRY_LENGTH_MAX 64

FILE *log_file = NULL;

GtkWidget *window;
GtkWidget *bar;
GtkWidget *label;
GtkWidget *combo;
GtkWidget *button;
GtkWidget *button_label;
gchar *dev;
GtkStatusIcon *tray_icon;

/* Configuration globals */
typedef enum { UNIT_AUTO
             , UNIT_BYTE
             , UNIT_KBYTE
             , UNIT_MBYTE
             , UNIT_GBYTE
             } unit_t;
unit_t unit = UNIT_AUTO;
unsigned long long bytes = 0;
unsigned char started = 0;
unsigned long long soft_limit = SOFT_LIMIT;
unsigned long long hard_limit = HARD_LIMIT;
GMutex* data_mutex = NULL; /* protects configuration globals */

void update_counter_label(gboolean from_thread);

void error_dialog (gchar *text, gboolean from_thread)
{
  GtkWidget *dialog =
    gtk_message_dialog_new (GTK_WINDOW (window),
                            GTK_DIALOG_MODAL
                            | GTK_DIALOG_DESTROY_WITH_PARENT,
                            GTK_MESSAGE_ERROR,
                            GTK_BUTTONS_OK,
                            "%s",text);
  if (from_thread)
    gdk_threads_enter();
  gtk_dialog_run (GTK_DIALOG (dialog));
  if (from_thread)
    gdk_threads_leave();
  gtk_widget_destroy (dialog);
}

static void tray_show(GtkMenuItem *item, gpointer window) 
{
  gtk_widget_show(GTK_WIDGET(window));
  gtk_window_deiconify(GTK_WINDOW(window));    
}

static void tray_quit(GtkMenuItem *item, gpointer user_data) 
{
    gtk_main_quit();
}

static void tray_icon_activated(GObject *trayIcon, gpointer window)
{
  gtk_widget_show(GTK_WIDGET(window));
  gtk_window_deiconify(GTK_WINDOW(window));
}

static void tray_icon_popup(GtkStatusIcon *status_icon, guint button, guint32 activate_time, gpointer popUpMenu)
{
  gtk_menu_popup(GTK_MENU(popUpMenu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static gboolean window_state_event (GtkWidget *widget, GdkEventWindowState *event, gpointer trayIcon)
{
  if (    event->changed_mask == GDK_WINDOW_STATE_ICONIFIED
       && (    event->new_window_state == GDK_WINDOW_STATE_ICONIFIED
            || event->new_window_state == (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_MAXIMIZED)
          )
     )
  {
      gtk_widget_hide (GTK_WIDGET(widget));
      gtk_status_icon_set_visible(GTK_STATUS_ICON(trayIcon), TRUE);
  }
  else if (    event->changed_mask == GDK_WINDOW_STATE_WITHDRAWN
            && (    event->new_window_state == GDK_WINDOW_STATE_ICONIFIED
                 || event->new_window_state == (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_MAXIMIZED)
               )
          )
  {
      gtk_status_icon_set_visible(GTK_STATUS_ICON(trayIcon), FALSE);
  }
  return TRUE;
}


/*
 * VIEW callbacks
 */

static void set_unit( gpointer   data,
                      GtkWidget *widget )
{
  g_mutex_lock (data_mutex);

  if (strcmp((const char *)data, "Auto") == 0) {
    unit = UNIT_AUTO;
  } else if (strcmp((const char *)data, "Byte") == 0) {
    unit = UNIT_BYTE;
  } else if (strcmp((const char *)data, "kByte") == 0) {
    unit = UNIT_KBYTE;
  } else if (strcmp((const char *)data, "MByte") == 0) {
    unit = UNIT_MBYTE;
  } else { /* GByte */
    unit = UNIT_GBYTE;
  }

  g_mutex_unlock (data_mutex);

  update_counter_label (FALSE);
}

/*
 * SETTINGS callbacks
 */

static void set_limits(gpointer data, GtkWidget *widget)
{
  gint response;
  GtkWidget *dialog;
  GtkWidget *hbox;
  GtkWidget *entry;
  GtkWidget *label;
  gboolean hard;
  gchar text[ENTRY_LENGTH_MAX];

  g_mutex_lock (data_mutex);
  if (strcmp (data, "Soft limit") == 0) {
    hard = FALSE;
    sprintf (text, "%llu", soft_limit);
  } else {
    hard = TRUE;
    sprintf (text, "%llu", hard_limit);
  }
  g_mutex_unlock (data_mutex);

  dialog = gtk_dialog_new_with_buttons (data,
					GTK_WINDOW (window),
					GTK_DIALOG_MODAL
          | GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_STOCK_OK,
					GTK_RESPONSE_OK,
          GTK_STOCK_CANCEL,
          GTK_RESPONSE_CANCEL,
					NULL);
  hbox = gtk_hbox_new (FALSE, 8);
  gtk_container_set_border_width (GTK_CONTAINER (hbox), 8);
  gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), hbox, FALSE, FALSE, 0);

  label = gtk_label_new (data);
  gtk_box_pack_start (GTK_BOX (hbox),
                      label, FALSE, FALSE, 0);

  entry = gtk_entry_new ();
  gtk_entry_set_max_length (GTK_ENTRY (entry), ENTRY_LENGTH_MAX);
  gtk_entry_set_text (GTK_ENTRY (entry), text);
  gtk_box_pack_start (GTK_BOX (hbox),
                      entry, FALSE, FALSE, 0);
  gtk_widget_show_all (GTK_DIALOG (dialog)->vbox);
  
  response = gtk_dialog_run (GTK_DIALOG (dialog));

  if (response == GTK_RESPONSE_OK) {
    const char *value = gtk_entry_get_text (GTK_ENTRY (entry));
    unsigned long long limit = (unsigned long long) strtod (value, NULL);
    g_mutex_lock (data_mutex);
    if (hard) {
      hard_limit = limit;
    } else {
      soft_limit = limit;
    }
    g_mutex_unlock (data_mutex);
  }

  gtk_widget_destroy (dialog);
}



/*
 * HELP callbacks
 */

static void help_about(gpointer data, GtkWidget *widget)
{
  const gchar *authors[] = {
    "Lajos Zaccomer (lajos@zaccomer.org)\n\n"
    "The code was written based on the official GTK+ tutorial at "
    "http://library.gnome.org/devel/gtk-tutorial/stable/\n"
    "Thanx to Ranjeetsih for his description of tray icon use on "
    "http://www.codeproject.com/KB/cross-platform/GTKTrayIcon.aspx"
    , NULL
  };

  gtk_show_about_dialog (GTK_WINDOW (window),
		 "name", "Traffic Counter",
		 "version", VERSION,
		 "authors", authors,
     "title", "About Traffic Counter",
		 NULL);
}


void update_counter_label(gboolean from_thread)
{
  #define TEXTBUFLEN 128
  char *textbuf;
  guint devlen = strlen(dev);  
  char *traytext = (char *)malloc(devlen + 2 + TEXTBUFLEN);

  if (traytext == NULL) {
    g_print("error: out of memory, unable to update counter display\n");
    return;
  }

  sprintf(traytext, "%s: ", dev);
  textbuf = traytext + devlen + 2;

  if (data_mutex) g_mutex_lock (data_mutex);
  switch (unit) {
    case UNIT_AUTO:
      if (bytes < 1000)
        sprintf(textbuf, "%llu Byte", bytes);
      else if (bytes < 1000000)
        sprintf(textbuf, "%.3f kByte", (double)bytes / 1000.0);
      else if (bytes < 1000000000)
        sprintf(textbuf, "%.3f MByte", (double)bytes / 1000000.0);
      else
        sprintf(textbuf, "%.3f GByte", (double)bytes / 1000000000.0);
      break;
    case UNIT_BYTE:
      sprintf(textbuf, "%llu Byte", bytes);
      break;
    case UNIT_KBYTE:
      sprintf(textbuf, "%.3f KByte", (double)bytes / 1000.0);
      break;
    case UNIT_MBYTE:
      sprintf(textbuf, "%.3f MByte", (double)bytes / 1000000.0);
      break;
    case UNIT_GBYTE:
      sprintf(textbuf, "%.3f GByte", (double)bytes / 1000000000.0);
      break;
  }
  if (data_mutex) g_mutex_unlock (data_mutex);

  if (bytes > hard_limit) {
    gtk_info_bar_set_message_type (GTK_INFO_BAR (bar), GTK_MESSAGE_ERROR);
  } else if (bytes > soft_limit) {
    gtk_info_bar_set_message_type (GTK_INFO_BAR (bar), GTK_MESSAGE_WARNING);
  } 

  gtk_status_icon_set_tooltip (tray_icon, traytext);

  if (from_thread)
    gdk_threads_enter();
  gtk_label_set_text( GTK_LABEL (label), textbuf);
  if (from_thread)
    gdk_threads_leave();
}

void got_packet(u_char *args, const struct pcap_pkthdr *header,
                const u_char *packet)
{
  g_mutex_lock (data_mutex);
  bytes += header->len;
  if (bytes > hard_limit) {
    g_print ("DEBUG: beyond hard limit\n");
  } else if (bytes > soft_limit) {
    g_print ("DEBUG: beyond soft limit\n");
  }
  g_mutex_unlock (data_mutex);
   
  g_print("packet captured: len = %u\n", header->len);
  
  update_counter_label(TRUE);
}


static void *counter(void *arg)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  struct pcap_pkthdr header;	/* The header that pcap gives us */
	const u_char *packet;		/* The actual packet */
  pcap_t *handle = NULL;  
  dev = gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo));

  if (dev == NULL) {
    error_dialog("You either have no device or "
                 "root (administrative) priviliges.",
                 TRUE);
    gdk_threads_enter();
    g_print( "stop counting\n");
    gtk_label_set_text( GTK_LABEL (button_label), "Start");
    g_mutex_lock (data_mutex);
    started = 0;
    g_mutex_unlock (data_mutex);
    gdk_threads_leave();
    g_thread_exit (NULL);
  }

  handle = pcap_open_live(dev, BUFSIZ, 1, 0, errbuf);
  if (handle == NULL) {
    fprintf(stderr, "Couldn't open device %s: %s\n", dev, errbuf);
    started = 0;
    return NULL;
  }
  g_print("handle = %p\n", handle);
  
  for (;;) {
    packet = pcap_next(handle, &header);

    g_mutex_lock (data_mutex);
    if (!started) {
      pcap_close(handle);
      g_mutex_unlock (data_mutex);
      g_thread_exit (NULL);
    }
    g_mutex_unlock (data_mutex);

    got_packet(NULL, &header, packet);
  }

  g_print("after pcap_loop\n");

  return NULL;
}



static void start(GtkWidget *widget, gpointer data)
{
  GError *error = NULL;
  time_t t = time(NULL);

  if (started) {
    g_print( "stop counting\n");
    gtk_label_set_text( GTK_LABEL (button_label), "Start");
    g_mutex_lock (data_mutex);
    started = 0;
    g_mutex_unlock (data_mutex);
	gtk_widget_set_sensitive (GTK_WIDGET(data), TRUE);

    (void) fprintf (log_file, "%s\tSTOP: bytes = %llu\n",
                    asctime(localtime(&t)), bytes);
  } else {
    g_print( "start counting\n");
    gtk_label_set_text( GTK_LABEL (button_label), "Stop");
    g_mutex_lock (data_mutex);
    started = 1;
    g_mutex_unlock (data_mutex);
    gtk_widget_set_sensitive (GTK_WIDGET(data), FALSE);

    (void) fprintf (log_file, "%s\tSTART: bytes = %llu\n",
                    asctime(localtime(&t)), bytes);

    if (!g_thread_create(&counter, NULL, FALSE, &error)) {
      g_printerr ("Failed to create counter thread: %s\n", error->message);
    }
  }
}

static gboolean reset(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  time_t t = time(NULL);

  g_mutex_lock (data_mutex);
  bytes = 0;
  g_mutex_unlock (data_mutex);

  (void) fprintf (log_file, "%s\tRESET: bytes = %llu\n",
                  asctime(localtime(&t)), bytes);

  gtk_info_bar_set_message_type (GTK_INFO_BAR (bar), GTK_MESSAGE_INFO);
  update_counter_label(FALSE);

  return FALSE;
}


static gboolean delete_event(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  FILE *f;
  time_t t = time(NULL);

  if (chdir(getenv("HOME"))) {
    g_print("Unable to change directory to %s\n", getenv("HOME"));
  }
  f = fopen (RCNAME, "w+");
  if (f == NULL) {
    error_dialog("Unable to open .trcntrc file "
                 "and save any changes.", FALSE);
  } else {
    gint ifx = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));
    fprintf (f, "bytes=%llu\n", bytes);
    fprintf (f, "unit=%u\n", unit);
    fprintf (f, "device=%d\n", ifx < 0 ? 0 : ifx);
    fprintf (f, "softlimit=%llu\n", soft_limit);
    fprintf (f, "hardlimit=%llu\n", hard_limit);
    fclose (f);
  }
  (void) fprintf (log_file, "%s\tQUIT: bytes = %llu\n",
                  asctime(localtime(&t)), bytes);
  fclose (log_file);
  gtk_main_quit ();
  return FALSE;
}



int main(int argc, char *argv[])
{
  GtkWidget *vbox;
  GtkWidget *hbox;
  GtkWidget *menu_bar;
  GtkWidget *menu;
  GtkWidget *item;
  GSList *group;
  GtkWidget *tray_menu, *tray_menu_item_show, *tray_menu_item_quit;

  pcap_if_t *iface = NULL;
  pcap_if_t *iflst = NULL;
  char errbuf[PCAP_ERRBUF_SIZE];
  gint ifx = 0;
  FILE *f = NULL;

  /* read configuration */  
  if (chdir(getenv("HOME"))) {
    g_print("Unable to change directory to %s\n", getenv("HOME"));
  }
  f = fopen (RCNAME, "r");
  if (f) {
    fscanf (f, "bytes=%llu\n", &bytes);
    fscanf (f, "unit=%u\n", &unit);
    fscanf (f, "device=%d\n", &ifx);
    if (ifx < 0) ifx = 0;
    fscanf (f, "softlimit=%llu\n", &soft_limit);
    fscanf (f, "hardlimit=%llu\n", &hard_limit);
    fclose (f);
  } else {
    g_print ("rc file is not found\n");
  }

  /* open logfile */
  log_file = fopen (LOGNAME, "a");
  g_print ("log file ptr = %p\n", log_file);
  if (log_file == NULL) {
    g_print ("logfile cannot be opened\n");
  }

  g_thread_init(NULL);
  gdk_threads_init();
  gtk_init (&argc, &argv);
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_screen(GTK_WINDOW (window),
  gtk_widget_get_screen(window));
  gtk_window_set_title (GTK_WINDOW (window), "Traffic counter");
  gtk_container_set_border_width (GTK_CONTAINER (window), 8);
	
  g_signal_connect (G_OBJECT (window), "delete_event",
	                  G_CALLBACK (delete_event), NULL);

  vbox = gtk_vbox_new (FALSE, 0);
  gtk_container_add (GTK_CONTAINER (window), vbox);

  /*
   * WINDOW MENU;
   */

  menu_bar = gtk_menu_bar_new ();
  gtk_box_pack_start (GTK_BOX(vbox), menu_bar, TRUE, TRUE, 0);

  /* MENU::FILE */
  menu = gtk_menu_new ();
  item = gtk_menu_item_new_with_label ("Quit");
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (delete_event),
                            (gpointer) "file.quit");
  item = gtk_menu_item_new_with_label ("File");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
  gtk_menu_bar_append (GTK_MENU_BAR (menu_bar), item);

  /*
   * MENU::VIEW
   */
  menu = gtk_menu_new ();
  group = NULL;
  item = gtk_radio_menu_item_new_with_label (group, "Auto");
  group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_unit),
                            (gpointer) "Auto");
  item = gtk_radio_menu_item_new_with_label (group, "Byte");
  group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_unit),
                            (gpointer) "Byte");
  item = gtk_radio_menu_item_new_with_label (group, "kByte");
  group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_unit),
                            (gpointer) "kByte");
  item = gtk_radio_menu_item_new_with_label (group, "MByte");
  group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_unit),
                            (gpointer) "MByte");
  item = gtk_radio_menu_item_new_with_label (group, "GByte");
  group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_unit),
                            (gpointer) "GByte");

  item = gtk_menu_item_new_with_label ("Unit");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);

  menu = gtk_menu_new ();
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

  item = gtk_menu_item_new_with_label ("View");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
  gtk_menu_bar_append (GTK_MENU_BAR (menu_bar), item);

  /*
   * MENU::SETTINGS
   */
  menu = gtk_menu_new ();
  item = gtk_menu_item_new_with_label ("Soft limit");
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_limits),
                            (gpointer) "Soft limit");
  item = gtk_menu_item_new_with_label ("Hard limit");
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (set_limits),
                            (gpointer) "Hard limit");
  item = gtk_menu_item_new_with_label ("Settings");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
  gtk_menu_bar_append (GTK_MENU_BAR (menu_bar), item);

  
  /* MENU::HELP */
  menu = gtk_menu_new ();
  item = gtk_menu_item_new_with_label ("About");
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
  g_signal_connect_swapped (G_OBJECT (item), "activate",
                            G_CALLBACK (help_about),
                            (gpointer) "help.about");
  item = gtk_menu_item_new_with_label ("Help");
  gtk_menu_item_right_justify ( GTK_MENU_ITEM (item));
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
  gtk_menu_bar_append (GTK_MENU_BAR (menu_bar), item);


  /*
   * WINDOW BODY
   */

  combo = gtk_combo_box_new_text();
  gtk_box_pack_start (GTK_BOX(vbox), combo, TRUE, TRUE, 0);

  if (pcap_findalldevs(&iflst, errbuf) == -1) {
		fprintf(stderr, "Couldn't create device list: %s\n", errbuf);
    return 3;
  }

  fprintf(stderr, "The following interfaces are available:\n");

  iface = iflst;
  while (iface) {
    gtk_combo_box_append_text (GTK_COMBO_BOX(combo), iface->name);
    fprintf(stderr, "  %s appended\n", iface->name);
    iface = iface->next;
  }
  pcap_freealldevs(iflst);

  gtk_combo_box_set_active (GTK_COMBO_BOX(combo), ifx);

  /* Counter */
  bar = gtk_info_bar_new ();
  gtk_info_bar_set_message_type (GTK_INFO_BAR (bar), GTK_MESSAGE_INFO);
  label = gtk_label_new ("0 bytes");
  gtk_box_pack_start (GTK_BOX (gtk_info_bar_get_content_area (GTK_INFO_BAR (bar))), label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX(vbox), bar, TRUE, TRUE, 0);

  hbox = gtk_hbox_new (TRUE, 10);
  gtk_box_pack_start (GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  /* Start/Stop button */
  button = gtk_button_new();
  button_label = gtk_label_new("Start");
  gtk_container_add (GTK_CONTAINER (button), button_label);
  gtk_box_pack_start (GTK_BOX(hbox), button, TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (button), "clicked",
		                G_CALLBACK (start), (gpointer) combo);

  /* Counter reset */
  button = gtk_button_new_with_label("Reset");
  gtk_box_pack_start (GTK_BOX(hbox), button, TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (button), "clicked",
		                G_CALLBACK (reset), (gpointer) NULL);

  /* Quit */
  button = gtk_button_new_with_label("Quit");
  gtk_box_pack_start (GTK_BOX(hbox), button, TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (button), "clicked",
		                G_CALLBACK (delete_event), (gpointer) NULL);

  /* Tray icon */
  tray_icon = gtk_status_icon_new_from_stock (GTK_STOCK_GO_DOWN);
  tray_menu = gtk_menu_new();
  tray_menu_item_show = gtk_menu_item_new_with_label ("Show");
  tray_menu_item_quit = gtk_menu_item_new_with_label ("Quit");
  g_signal_connect (G_OBJECT (tray_menu_item_show), "activate", G_CALLBACK (tray_show), window);
  g_signal_connect (G_OBJECT (tray_menu_item_quit), "activate", G_CALLBACK (tray_quit), NULL);
  gtk_menu_shell_append (GTK_MENU_SHELL (tray_menu), tray_menu_item_show);
  gtk_menu_shell_append (GTK_MENU_SHELL (tray_menu), tray_menu_item_quit);
  gtk_widget_show_all (tray_menu);
  g_signal_connect(GTK_STATUS_ICON (tray_icon), "activate", GTK_SIGNAL_FUNC (tray_icon_activated), window);
  g_signal_connect(GTK_STATUS_ICON (tray_icon), "popup-menu", GTK_SIGNAL_FUNC (tray_icon_popup), tray_menu);
  gtk_status_icon_set_visible(tray_icon, FALSE); //set icon initially invisible
  g_signal_connect (G_OBJECT (window), "window-state-event", G_CALLBACK (window_state_event), tray_icon);


  dev = gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo));
  update_counter_label(FALSE);

  gtk_widget_show_all (window);

  data_mutex = g_mutex_new ();
  
  gdk_threads_enter();
  gtk_main();
  gdk_threads_leave();

  return(0);
}

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
  return main(__argc, __argv);
}
#endif /* WIN32 */

