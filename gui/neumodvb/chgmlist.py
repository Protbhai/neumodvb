#!/usr/bin/python3
# Neumo dvb (C) 2019-2021 deeptho@gmail.com
# Copyright notice:
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#
import wx
import wx.grid
import sys
import os
import copy
from collections import namedtuple, OrderedDict
import numbers
import datetime
from dateutil import tz
import regex as re

from neumodvb.util import setup, lastdot, dtdebug, dterror
from neumodvb import neumodbutils
from neumodvb.neumo_dialogs_gui import ChannelNoDialog_
from neumodvb.neumolist import NeumoTable, NeumoGridBase, GridPopup, screen_if_t

import pychdb

class ChgmNoDialog(ChannelNoDialog_):
    def __init__(self, parent, basic, *args, **kwds):
        self.parent= parent
        self.timeout = 1000
        if "initial_chno" in kwds:
            initial_chno = str(kwds['initial_chno'])
            del kwds['initial_chno']
        else:
            initial_chno = None
        kwds["style"] =  kwds.get("style", 0) | wx.NO_BORDER
        super().__init__(parent, basic, *args, **kwds)
        if initial_chno is not None:
            self.chno.ChangeValue(initial_chno)
            self.chno.SetInsertionPointEnd()
        self.timer= wx.Timer(owner=self , id =1)
        self.Bind(wx.EVT_TIMER, self.OnTimer)
        self.timer.StartOnce(milliseconds=self.timeout)
        self.chno.Bind(wx.EVT_CHAR, self.CheckCancel)

    def CheckCancel(self, event):
        if event.GetKeyCode() in [wx.WXK_ESCAPE, wx.WXK_CONTROL_C]:
            self.OnTimer(None, ret=wx.ID_CANCEL)
            event.Skip(False)
        event.Skip()

    def OnText(self, event):
        self.timer.Stop()
        self.timer.StartOnce(milliseconds=self.timeout)
        event.Skip()

    def OnTextEnter(self, event):
        self.OnTimer(None)
        event.Skip()

    def OnTimer(self, event, ret=wx.ID_OK):
        self.EndModal(ret)


def ask_channel_number(caller, initial_chno=None):
    if initial_chno is not None:
        initial_chno = str(initial_chno)
    dlg = ChgmNoDialog(caller, -1, "Chgm Number", initial_chno = initial_chno)
    val = dlg.ShowModal()
    chno = None
    if val == wx.ID_OK:
        try:
            chno = int(dlg.chno.GetValue())
        except:
            pass
    dlg.Destroy()
    return chno


class ChgmTable(NeumoTable):
    CD = NeumoTable.CD
    datetime_fn =  lambda x: datetime.datetime.fromtimestamp(x[1], tz=tz.tzlocal()).strftime("%Y-%m-%d %H:%M:%S")
    bool_fn = NeumoTable.bool_fn
    all_columns = \
        [CD(key='user_order', label='chno', basic=True, example="1000"),
         CD(key='chgm_order', label='lcn', basic=False, example="1000"),
         CD(key='k.channel_id', label='id', basic=False, example="1000"),
         CD(key='name',  label='Name', basic=True, example="Investigation discovery12345"),
         CD(key='media_mode',  label='media_mode', dfn=lambda x: lastdot(x), example="RADIO"),
         CD(key='service.mux.sat_pos', label='Sat', basic=True, dfn= lambda x: pychdb.sat_pos_str(x[1])),
         CD(key='service.mux.network_id',  label='nid'),
         CD(key='expired',  label='Expired',  dfn=bool_fn),
         CD(key='media_mode',  label='type', dfn=lambda x: lastdot(x)),
         CD(key='service.mux.ts_id',  label='tsid'),
         CD(key='service.mux.extra_id',  label='subid'),
         CD(key='service.service_id',  label='sid'),
         CD(key='mtime', label='Modified', dfn=datetime_fn, example="2020-12-29 18:35:01"),
         CD(key='icons',  label='', basic=False, dfn=bool_fn, example='1234'),
         ]

    def InitialRecord(self):
        chg, chgm = self.parent.CurrentChgAndChgm()
        dtdebug(f"INITIAL RECORD chdg={chg} chgm={chgm}")
        return chgm

    def __init__(self, parent, basic=False, *args, **kwds):
        initial_sorted_column = 'chgm_order'
        data_table= pychdb.chgm
        screen_getter = lambda txn, subfield: self.screen_getter_xxx(txn, subfield)

        super().__init__(*args, parent=parent, basic=basic, db_t=pychdb, data_table= data_table,
                         screen_getter = screen_getter,
                         record_t = pychdb.chgm.chgm,
                         initial_sorted_column = initial_sorted_column, **kwds)
        self.app = wx.GetApp()

    def __save_record__(self, txn, record):
        pychdb.put_record(txn, record)
        return record

    def screen_getter_xxx(self, txn, sort_field):
        match_data, matchers = self.get_filter_()
        if  self.parent.restrict_to_chg:
            chg, chgm = self.parent.CurrentChgAndChgm()
            ref = pychdb.chgm.chgm()
            ref.k.chg = chg.k
            txn = self.db.rtxn()
            screen = pychdb.chgm.screen(txn, sort_order=sort_field,
                                           key_prefix_type=pychdb.chgm.chgm_prefix.chg, key_prefix_data=ref,
                                        field_matchers=matchers, match_data = match_data)
            txn.abort()
        else:
            chg = None
            chgm = None
            screen = pychdb.chgm.screen(txn, sort_order=sort_field,
                                        field_matchers=matchers, match_data = match_data)
        self.screen=screen_if_t(screen, self.sort_order==2)

    def __new_record__(self):
        return self.record_t()

    def get_icons(self):
        return (self.app.bitmaps.encrypted_bitmap, self.app.bitmaps.expired_bitmap)

    def get_icon_sort_key(self):
        return 'encrypted'

    def get_icon_state(self, rowno, colno):
        col = self.columns[colno]
        chgm = self.GetRow(rowno)
        return ( chgm.encrypted, chgm.expired)

    def needs_highlight(self,chgm):
        e = self.app.frame.bouquet_being_edited
        if e is None:
            return False
        txn =self.db.rtxn()
        ret = pychdb.chg.contains_service(txn, e, chgm.service)
        txn.abort()
        return ret

class ChgmGridBase(NeumoGridBase):
    def __init__(self, basic, readonly, *args, **kwds):
        self.allow_all = True
        table = ChgmTable(self, basic)
        super().__init__(basic, readonly, table, *args, **kwds)
        self.sort_order = 0
        self.sort_column = None
        self.Bind(wx.EVT_KEY_DOWN, self.OnKeyDown)
        self.grid_specific_menu_items=['epg_record_menu_item']
        self.restrict_to_chg = None
        self.chgm = None

    def MoveToChno(self, chno):
        txn = wx.GetApp().chdb.rtxn()
        channel = pychdb.chgm.find_by_chgm_order(txn, chno)
        txn.abort()
        if channel is None:
            return
        row = self.table.screen.set_reference(channel)
        if row is not None:
            self.GoToCell(row, self.GetGridCursorCol())
            self.SelectRow(row)
            self.SetFocus()

    def OnShow(self, evt):
        self.chgm = None
        super().OnShow(evt)

    def OnToggleRecord(self, evt):
        row = self.GetGridCursorRow()
        return self.screen.record_at_row(row), None

    def OnKeyDown(self, evt):
        """
        After editing, move cursor right
        """
        keycode = evt.GetKeyCode()
        modifiers = evt.GetModifiers()
        is_ctrl = (modifiers & wx.ACCEL_CTRL)
        if keycode == wx.WXK_RETURN and not evt.HasAnyModifiers():
            self.MoveCursorRight(False)
            evt.Skip(False)
        else:
            evt.Skip(True)
        keycode = evt.GetUnicodeKey()
        if keycode == wx.WXK_RETURN and not evt.HasAnyModifiers():
            if self.EditMode():
                self.MoveCursorRight(False)
            else:
                row = self.GetGridCursorRow()
                chgm = self.table.GetRow(row)
                self.app.ServiceTune(chgm)
            evt.Skip(False)
        elif not self.EditMode() and not is_ctrl and IsNumericKey(keycode):
            self.MoveToChno(ask_channel_number(self, keycode- ord('0')))
        else:
            return

    def EditMode(self):
        return  self.GetParent().GetParent().edit_mode

    def SelectChg(self, chg):
        self.restrict_to_chg = chg
        self.chgm = None
        self.app.live_service_screen.set_chg_filter(chg)
        self.restrict_to_chg = self.app.live_service_screen.filter_chg
        wx.CallAfter(self.doit, None, self.chgm)

    def doit(self, evt, chgm):
        self.OnRefresh(evt, chgm)

    def CurrentChgAndChgm(self):
        if self.restrict_to_chg is None:
            chgm = self.app.live_service_screen.selected_chgm
            self.restrict_to_chg =  self.app.live_service_screen.filter_chg
            self.chgm = chgm
        else:
            if self.chgm is None:
                self.chgm = self.app.live_service_screen.selected_chgm
        return self.restrict_to_chg, self.chgm

    def CurrentGroupText(self):
        chg, chgm = self.CurrentChgAndChgm()
        if chg is None:
            return "All bouquets"
        return str(chg.name if len(chg.name)>0 else str(chg))


    def CmdTune(self, evt):
        dtdebug('CmdTune')
        rowno = self.GetGridCursorRow()
        chgm = self.table.GetRow(rowno)
        self.table.SaveModified()
        self.app.ServiceTune(chgm)

    def CmdBouquetAddService(self, evt):
        dtdebug('CmdBouquetAddService')
        row = self.GetGridCursorRow()
        chgm = self.table.screen.record_at_row(row)
        dtdebug(f'request to add channel {chgm} to {self.app.frame.bouquet_being_edited}')
        wtxn =  wx.GetApp().chdb.wtxn()
        pychdb.chg.toggle_channel_in_bouquet(wtxn, self.app.frame.bouquet_being_edited, chgm)
        wtxn.commit()
        self.table.OnModified()

    @property
    def CmdEditBouquetMode(self):
        if self.app.frame.bouquet_being_edited is None:
            return False #signal to neumomenu that item is disabled
        return self.app.frame.chggrid.CmdEditBouquetMode





def IsNumericKey(keycode):
    return keycode >= ord('0') and keycode <= ord('9')

class BasicChgmGrid(ChgmGridBase):
    def __init__(self, *args, **kwds):
        super().__init__(True, True, *args, **kwds)
        if False:
            self.SetSelectionMode(wx.grid.Grid.GridSelectionModes.GridSelectRows)
        else:
            self.SetSelectionMode(wx.grid.Grid.SelectRows)

class ChgmGrid(ChgmGridBase):
    def __init__(self, *args, **kwds):
        super().__init__(False, False, *args, **kwds)
