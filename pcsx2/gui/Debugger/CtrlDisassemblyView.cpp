/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "CtrlDisassemblyView.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/Debug.h"
#include "DebugTools/MipsAssembler.h"

#include "DebugEvents.h"
#include "BreakpointWindow.h"
#include "gui/AppConfig.h"
#include "System.h"
#include "DisassemblyDialog.h"

#include <wx/mstream.h>
#include <wx/clipbrd.h>
#include <wx/file.h>

#include "gui/Resources/Breakpoint_Active.h"
#include "gui/Resources/Breakpoint_Inactive.h"

wxBEGIN_EVENT_TABLE(CtrlDisassemblyView, wxWindow)
	EVT_PAINT(CtrlDisassemblyView::paintEvent)
	EVT_MOUSEWHEEL(CtrlDisassemblyView::mouseEvent)
	EVT_LEFT_DOWN(CtrlDisassemblyView::mouseEvent)
	EVT_LEFT_DCLICK(CtrlDisassemblyView::mouseEvent)
	EVT_RIGHT_DOWN(CtrlDisassemblyView::mouseEvent)
	EVT_RIGHT_UP(CtrlDisassemblyView::mouseEvent)
	EVT_MOTION(CtrlDisassemblyView::mouseEvent)
	EVT_KEY_DOWN(CtrlDisassemblyView::keydownEvent)
	EVT_CHAR(CtrlDisassemblyView::keydownEvent)
	EVT_SCROLLWIN_LINEUP(CtrlDisassemblyView::scrollbarEvent)
	EVT_SCROLLWIN_LINEDOWN(CtrlDisassemblyView::scrollbarEvent)
	EVT_SCROLLWIN_PAGEUP(CtrlDisassemblyView::scrollbarEvent)
	EVT_SCROLLWIN_PAGEDOWN(CtrlDisassemblyView::scrollbarEvent)
	EVT_SIZE(CtrlDisassemblyView::sizeEvent)
	EVT_SET_FOCUS(CtrlDisassemblyView::focusEvent)
	EVT_KILL_FOCUS(CtrlDisassemblyView::focusEvent)
wxEND_EVENT_TABLE()

enum DisassemblyMenuIdentifiers
{
	ID_DISASM_COPYADDRESS = 1,
	ID_DISASM_COPYINSTRUCTIONHEX,
	ID_DISASM_COPYINSTRUCTIONDISASM,
	ID_DISASM_DISASSEMBLETOFILE,
	ID_DISASM_ASSEMBLE,
	ID_DISASM_RUNTOHERE,
	ID_DISASM_SETPCTOHERE,
	ID_DISASM_TOGGLEBREAKPOINT,
	ID_DISASM_FOLLOWBRANCH,
	ID_DISASM_GOTOADDRESS,
	ID_DISASM_GOTOINMEMORYVIEW,
	ID_DISASM_KILLFUNCTION,
	ID_DISASM_RENAMEFUNCTION,
	ID_DISASM_REMOVEFUNCTION,
	ID_DISASM_ADDFUNCTION
};

class NonAutoSelectTextCtrl : public wxTextCtrl
{
public:
	NonAutoSelectTextCtrl(wxWindow* parent, wxWindowID id,
		const wxString& value = wxEmptyString,
		const wxPoint& pos = wxDefaultPosition,
		const wxSize& size = wxDefaultSize,
		long style = 0,
		const wxValidator& validator = wxDefaultValidator,
		const wxString& name = wxTextCtrlNameStr)
		: wxTextCtrl(parent, id, value, pos, size, style, validator, name)
	{
	}
#ifdef _WIN32

#define WM_GETDLGCODE 0x0087
#define DLGC_HASSETSEL 0x0008

	virtual WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
	{
		WXLRESULT result = wxTextCtrl::MSWWindowProc(nMsg, wParam, lParam);
		if (nMsg == WM_GETDLGCODE)
			result &= ~DLGC_HASSETSEL;
		return result;
	}
#endif
};

class TextEntryDialog : public wxDialog
{
public:
	TextEntryDialog(wxWindow* parent, wxString title, wxString defaultText)
		: wxDialog(parent, wxID_ANY, title)
	{
		wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

		textControl = new NonAutoSelectTextCtrl(this, wxID_ANY, L"", wxDefaultPosition, wxDefaultSize, 0);
		textControl->SetValue(defaultText);
		textControl->SetSelection(textControl->GetLastPosition(), textControl->GetLastPosition());
		sizer->Add(textControl, 0, wxEXPAND);

		wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
		okButton = new wxButton(this, wxID_OK, L"OK");
		okButton->SetDefault();
		cancelButton = new wxButton(this, wxID_CANCEL, L"Cancel");

		buttons->Add(okButton);
		buttons->Add(cancelButton);
		sizer->Add(buttons, 0, wxEXPAND);

		SetSizer(sizer);
		sizer->Layout();
		sizer->Fit(this);

		textControl->SetFocus();
		textControl->SetFocusFromKbd();
	}
	wxString getText() { return textControl->GetValue(); }

private:
	NonAutoSelectTextCtrl* textControl;
	wxButton* okButton;
	wxButton* cancelButton;
};

inline wxIcon _wxGetIconFromMemory(const unsigned char* data, int length)
{
	wxMemoryInputStream is(data, length);
	wxBitmap b = wxBitmap(wxImage(is, wxBITMAP_TYPE_ANY, -1), -1);
	wxIcon icon;
	icon.CopyFromBitmap(b);
	return icon;
}

CtrlDisassemblyView::CtrlDisassemblyView(wxWindow* parent, DebugInterface* _cpu)
	: wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_SIMPLE | wxVSCROLL)
	, cpu(_cpu)
{
	manager.setCpu(cpu);
	windowStart = 0x100000;
	rowHeight = getDebugFontHeight() + 2;
	charWidth = getDebugFontWidth();
	displaySymbols = true;
	visibleRows = 1;

	bpEnabled = _wxGetIconFromMemory(res_Breakpoint_Active::Data, res_Breakpoint_Active::Length);
	bpDisabled = _wxGetIconFromMemory(res_Breakpoint_Inactive::Data, res_Breakpoint_Inactive::Length);

	menu.Append(ID_DISASM_COPYADDRESS, L"Copy Address");
	menu.Append(ID_DISASM_COPYINSTRUCTIONHEX, L"Copy Instruction (Hex)");
	menu.Append(ID_DISASM_COPYINSTRUCTIONDISASM, L"Copy Instruction (Disasm)");
	menu.Append(ID_DISASM_DISASSEMBLETOFILE, L"Disassemble to File");
	menu.AppendSeparator();
	menu.Append(ID_DISASM_ASSEMBLE, L"Assemble Opcode(s)");
	menu.AppendSeparator();
	menu.Append(ID_DISASM_RUNTOHERE, L"Run to Cursor");
	menu.Append(ID_DISASM_SETPCTOHERE, L"Jump to Cursor");
	menu.Append(ID_DISASM_TOGGLEBREAKPOINT, L"Toggle Breakpoint");
	menu.Append(ID_DISASM_FOLLOWBRANCH, L"Follow Branch");
	menu.AppendSeparator();
	menu.Append(ID_DISASM_GOTOADDRESS, L"Go to Address");
	menu.Append(ID_DISASM_GOTOINMEMORYVIEW, L"Go to in Memory View");
	menu.AppendSeparator();
	menu.Append(ID_DISASM_ADDFUNCTION, L"Add Function Here");
	menu.Append(ID_DISASM_RENAMEFUNCTION, L"Rename Function");
	menu.Append(ID_DISASM_REMOVEFUNCTION, L"Remove Function");
	menu.Bind(wxEVT_MENU, &CtrlDisassemblyView::onPopupClick, this);

	SetScrollbar(wxVERTICAL, 100, 1, 201, true);
	SetDoubleBuffered(true);
	calculatePixelPositions();
	setCurAddress(windowStart);
}

#ifdef _WIN32
WXLRESULT CtrlDisassemblyView::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
	switch (nMsg)
	{
		case 0x0104: // WM_SYSKEYDOWN, make f10 usable
			if (wParam == 0x79) // f10
			{
				postEvent(debEVT_STEPOVER, 0);
				return 0;
			}
			break;
	}

	return wxWindow::MSWWindowProc(nMsg, wParam, lParam);
}
#endif

void CtrlDisassemblyView::scanFunctions()
{
	if (!cpu->isAlive())
		return;

	manager.analyze(windowStart, manager.getNthNextAddress(windowStart, visibleRows) - windowStart);
}

void CtrlDisassemblyView::postEvent(wxEventType type, wxString text)
{
	wxCommandEvent event(type, GetId());
	event.SetEventObject(this);
	event.SetClientData(cpu);
	event.SetString(text);
	wxPostEvent(this, event);
}

void CtrlDisassemblyView::postEvent(wxEventType type, int value)
{
	wxCommandEvent event(type, GetId());
	event.SetEventObject(this);
	event.SetClientData(cpu);
	event.SetInt(value);
	wxPostEvent(this, event);
}

void CtrlDisassemblyView::paintEvent(wxPaintEvent& evt)
{
	wxPaintDC dc(this);
	render(dc);
}

void CtrlDisassemblyView::redraw()
{
	wxClientDC dc(this);
	render(dc);
}

bool CtrlDisassemblyView::getDisasmAddressText(u32 address, char* dest, bool abbreviateLabels, bool showData)
{
	if (displaySymbols)
	{
		const std::string addressSymbol = cpu->GetSymbolMap().GetLabelString(address);
		if (!addressSymbol.empty())
		{
			for (int k = 0; addressSymbol[k] != 0; k++)
			{
				// abbreviate long names
				if (abbreviateLabels && k == 16 && addressSymbol[k + 1] != 0)
				{
					*dest++ = '+';
					break;
				}
				*dest++ = addressSymbol[k];
			}
			*dest++ = ':';
			*dest = 0;
			return true;
		}
		else
		{
			sprintf(dest, "    %08X", address);
			return false;
		}
	}
	else
	{
		if (showData)
			sprintf(dest, "%08X %08X", address, cpu->read32(address));
		else
			sprintf(dest, "%08X", address);
		return false;
	}
}

wxColor scaleColor(wxColor color, float factor)
{
	unsigned char r = color.Red();
	unsigned char g = color.Green();
	unsigned char b = color.Blue();
	unsigned char a = color.Alpha();

	r = std::min(255, std::max((int)(r * factor), 0));
	g = std::min(255, std::max((int)(g * factor), 0));
	b = std::min(255, std::max((int)(b * factor), 0));

	return wxColor(r, g, b, a);
}

void CtrlDisassemblyView::drawBranchLine(wxDC& dc, std::map<u32, int>& addressPositions, BranchLine& line)
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);

	int winBottom = GetSize().GetHeight();

	int topY;
	int bottomY;
	if (line.first < windowStart)
	{
		topY = -1;
	}
	else if (line.first >= windowEnd)
	{
		topY = GetSize().GetHeight() + 1;
	}
	else
	{
		topY = addressPositions[line.first] + rowHeight / 2;
	}

	if (line.second < windowStart)
	{
		bottomY = -1;
	}
	else if (line.second >= windowEnd)
	{
		bottomY = GetSize().GetHeight() + 1;
	}
	else
	{
		bottomY = addressPositions[line.second] + rowHeight / 2;
	}

	if ((topY < 0 && bottomY < 0) || (topY > winBottom && bottomY > winBottom))
	{
		return;
	}

	// highlight line in a different color if it affects the currently selected opcode
	wxColor color;
	if (line.first == curAddress || line.second == curAddress)
	{
		color = wxColor(0xFF257AFA);
	}
	else
	{
		color = wxColor(0xFFFF3020);
	}

	wxPen pen = wxPen(color);
	dc.SetBrush(wxBrush(color));
	dc.SetPen(wxPen(color));

	int x = pixelPositions.arrowsStart + line.laneIndex * 8;

	if (topY < 0) // first is not visible, but second is
	{
		dc.DrawLine(x - 2, bottomY, x + 2, bottomY);
		dc.DrawLine(x + 2, bottomY, x + 2, 0);

		if (line.type == LINE_DOWN)
		{
			dc.DrawLine(x, bottomY - 4, x - 4, bottomY);
			dc.DrawLine(x - 4, bottomY, x + 1, bottomY + 5);
		}
	}
	else if (bottomY > winBottom) // second is not visible, but first is
	{
		dc.DrawLine(x - 2, topY, x + 2, topY);
		dc.DrawLine(x + 2, topY, x + 2, winBottom);

		if (line.type == LINE_UP)
		{
			dc.DrawLine(x, topY - 4, x - 4, topY);
			dc.DrawLine(x - 4, topY, x + 1, topY + 5);
		}
	}
	else
	{ // both are visible
		if (line.type == LINE_UP)
		{
			dc.DrawLine(x - 2, bottomY, x + 2, bottomY);
			dc.DrawLine(x + 2, bottomY, x + 2, topY);
			dc.DrawLine(x + 2, topY, x - 4, topY);

			dc.DrawLine(x, topY - 4, x - 4, topY);
			dc.DrawLine(x - 4, topY, x + 1, topY + 5);
		}
		else
		{
			dc.DrawLine(x - 2, topY, x + 2, topY);
			dc.DrawLine(x + 2, topY, x + 2, bottomY);
			dc.DrawLine(x + 2, bottomY, x - 4, bottomY);

			dc.DrawLine(x, bottomY - 4, x - 4, bottomY);
			dc.DrawLine(x - 4, bottomY, x + 1, bottomY + 5);
		}
	}
}

int CtrlDisassemblyView::getBackgroundColor(unsigned int address)
{
	u32 colors[6] = {0xFFe0FFFF, 0xFFFFe0e0, 0xFFe8e8FF, 0xFFFFe0FF, 0xFFe0FFe0, 0xFFFFFFe0};
	int n = cpu->GetSymbolMap().GetFunctionNum(address);
	if (n == -1)
		return 0xFFFFFFFF;
	return colors[n % 6];
}

std::set<std::string> CtrlDisassemblyView::getSelectedLineArguments()
{
	std::set<std::string> args;

	DisassemblyLineInfo line = DisassemblyLineInfo();
	for (u32 addr = selectRangeStart; addr < selectRangeEnd; addr += 4)
	{
		manager.getLine(addr, displaySymbols, line);
		size_t p = 0, nextp = line.params.find(',');
		while (nextp != line.params.npos)
		{
			args.insert(line.params.substr(p, nextp - p));
			p = nextp + 1;
			nextp = line.params.find(',', p);
		}

		if (p < line.params.size())
		{
			args.insert(line.params.substr(p));
		}

		// check for registers in memory opcodes
		p = line.params.find('(');
		nextp = line.params.find(')');
		if (p != line.params.npos && nextp != line.params.npos && nextp > p)
		{
			args.insert(line.params.substr(p + 1, nextp - p - 1));
		}
	}

	return args;
}

void CtrlDisassemblyView::drawArguments(wxDC& dc, const DisassemblyLineInfo& line, int x, int y, wxColor& textColor,
	const std::set<std::string>& currentArguments)
{
	if (line.params.empty())
		return;

	// Don't highlight the selected lines.
	if (isInInterval(selectRangeStart, selectRangeEnd - selectRangeStart, line.info.opcodeAddress))
	{
		dc.DrawText(wxString(line.params.c_str(), wxConvUTF8), x, y);
		return;
	}

	wxColor highlightedColor = wxColor(textColor == 0xFF0000FF ? 0xFFAABB77 : 0xFFAABB00);

	size_t p = 0, nextp = line.params.find(',');
	while (nextp != line.params.npos)
	{
		const std::string arg = line.params.substr(p, nextp - p);
		if (currentArguments.find(arg) != currentArguments.end() && textColor != 0xFFFFFFFF)
		{
			dc.SetTextForeground(highlightedColor);
		}
		dc.DrawText(wxString(arg.c_str(), wxConvUTF8), x, y);
		x += arg.size() * charWidth;

		p = nextp + 1;
		nextp = line.params.find(',', p);

		dc.SetTextForeground(textColor);
		dc.DrawText(L",", x, y);
		x += charWidth;
	}
	if (p < line.params.size())
	{
		const std::string arg = line.params.substr(p);
		if (currentArguments.find(arg) != currentArguments.end() && textColor != 0xFFFFFFFF)
		{
			dc.SetTextForeground(highlightedColor);
		}
		dc.DrawText(wxString(arg.c_str(), wxConvUTF8), x, y);
		dc.SetTextForeground(textColor);
	}
}

void CtrlDisassemblyView::render(wxDC& dc)
{
	// init stuff
	int totalWidth, totalHeight;
	GetSize(&totalWidth, &totalHeight);
	visibleRows = totalHeight / rowHeight;

	// clear background
	wxColor white = wxColor(0xFFFFFFFF);

	dc.SetBrush(wxBrush(white));
	dc.SetPen(wxPen(white));

	int width, height;
	dc.GetSize(&width, &height);
	dc.DrawRectangle(0, 0, width, height);

	if (!cpu->isAlive())
		return;

	wxFont font = pxGetFixedFont(8);
	wxFont boldFont = pxGetFixedFont(8, wxFONTWEIGHT_BOLD);
	font.SetPixelSize(wxSize(charWidth, rowHeight - 2));
	boldFont.SetPixelSize(wxSize(charWidth, rowHeight - 2));

	bool hasFocus = wxWindow::FindFocus() == this;

	std::map<u32, int> addressPositions;

	unsigned int address = windowStart;

	const std::set<std::string> currentArguments = getSelectedLineArguments();
	DisassemblyLineInfo line = DisassemblyLineInfo();
	for (int i = 0; i < visibleRows + 1; i++)
	{
		manager.getLine(address, displaySymbols, line);

		int rowY1 = rowHeight * i;

		addressPositions[address] = rowY1;

		wxColor backgroundColor = wxColor(getBackgroundColor(address));
		wxColor textColor = wxColor(0xFF000000);

		if (isInInterval(address, line.totalSize, cpu->getPC()))
		{
			backgroundColor = scaleColor(backgroundColor, 1.05f);
		}

		if (address >= selectRangeStart && address < selectRangeEnd)
		{
			if (hasFocus)
			{
				backgroundColor = address == curAddress ? 0xFFFF8822 : 0xFFFF9933;
				textColor = 0xFFFFFFFF;
			}
			else
			{
				backgroundColor = 0xFFC0C0C0;
			}
		}

		// display whether the condition of a branch is met
		if (line.info.isConditional && address == cpu->getPC())
		{
			line.params += line.info.conditionMet ? "  ; true" : "  ; false";
		}

		// draw background
		dc.SetBrush(wxBrush(backgroundColor));
		dc.SetPen(wxPen(backgroundColor));
		dc.DrawRectangle(0, rowY1, totalWidth, rowHeight);

		// display address/symbol
		bool enabled;
		if (CBreakPoints::IsAddressBreakPoint(cpu->getCpuType(), address, &enabled))
		{
			if (enabled)
				textColor = 0x0000FF;
			int yOffset = std::max(-1, (rowHeight - 14 + 1) / 2);
			dc.DrawIcon(enabled ? bpEnabled : bpDisabled, 2, rowY1 + 1 + yOffset);
		}

		dc.SetTextForeground(textColor);

		char addressText[64];
		getDisasmAddressText(address, addressText, true, line.type == DISTYPE_OPCODE);

		dc.SetFont(font);
		dc.DrawText(wxString(addressText, wxConvUTF8), pixelPositions.addressStart, rowY1 + 2);
		drawArguments(dc, line, pixelPositions.argumentsStart, rowY1 + 2, textColor, currentArguments);

		if (isInInterval(address, line.totalSize, cpu->getPC()))
			dc.DrawText(L"\u25A0", pixelPositions.opcodeStart - (charWidth + 1), rowY1);

		dc.SetFont(boldFont);
		dc.DrawText(wxString(line.name.c_str(), wxConvUTF8), pixelPositions.opcodeStart, rowY1 + 2);

		address += line.totalSize;
	}

	std::vector<BranchLine> branchLines = manager.getBranchLines(windowStart, address - windowStart);
	for (size_t i = 0; i < branchLines.size(); i++)
	{
		drawBranchLine(dc, addressPositions, branchLines[i]);
	}
}

void CtrlDisassemblyView::gotoAddress(u32 addr)
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);
	u32 newAddress = manager.getStartAddress(addr);

	if (newAddress < windowStart || newAddress >= windowEnd)
	{
		windowStart = manager.getNthPreviousAddress(newAddress, visibleRows / 2);
	}

	setCurAddress(addr);
	scanFunctions();
	redraw();
}

void CtrlDisassemblyView::scrollAddressIntoView()
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);

	if (curAddress < windowStart)
		windowStart = curAddress;
	else if (curAddress >= windowEnd)
		windowStart = manager.getNthPreviousAddress(curAddress, visibleRows - 1);

	scanFunctions();
}

void CtrlDisassemblyView::calculatePixelPositions()
{
	pixelPositions.addressStart = 16;
	pixelPositions.opcodeStart = pixelPositions.addressStart + 18 * charWidth;
	pixelPositions.argumentsStart = pixelPositions.opcodeStart + 9 * charWidth;
	pixelPositions.arrowsStart = pixelPositions.argumentsStart + 30 * charWidth;
}


void CtrlDisassemblyView::followBranch()
{
	DisassemblyLineInfo line = DisassemblyLineInfo();
	manager.getLine(curAddress, true, line);

	if (line.type == DISTYPE_OPCODE || line.type == DISTYPE_MACRO)
	{
		if (line.info.isBranch)
		{
			jumpStack.push_back(curAddress);
			gotoAddress(line.info.branchTarget);
		}
		else if (line.info.hasRelevantAddress)
		{
			// well, not  exactly a branch, but we can do something anyway
			postEvent(debEVT_GOTOINMEMORYVIEW, line.info.releventAddress);
		}
	}
	else if (line.type == DISTYPE_DATA)
	{
		// jump to the start of the current line
		postEvent(debEVT_GOTOINMEMORYVIEW, curAddress);
	}
}

void CtrlDisassemblyView::assembleOpcode(u32 address, std::string defaultText)
{
	u32 encoded;

	if (!cpu->isCpuPaused())
	{
		wxMessageBox(L"Cannot change code while the core is running", L"Error.", wxICON_ERROR);
		return;
	}

	TextEntryDialog entry(this, L"Assemble opcode", wxString(defaultText.c_str(), wxConvUTF8));
	entry.Layout();

	if (entry.ShowModal() != wxID_OK)
		return;

	wxString op = entry.getText();
	std::string errorText;
	bool result = MipsAssembleOpcode(op.To8BitData(), cpu, address, encoded, errorText);
	if (result)
	{
		SysClearExecutionCache();

		if ((selectRangeEnd - selectRangeStart) > 4)
		{
			for (u32 addr = selectRangeStart; addr < selectRangeEnd; addr += 0x4)
			{
				cpu->write32(addr, encoded);
			}
		}
		else
		{
			cpu->write32(address, encoded);
		}

		redraw();
	}
	else
	{
		wxMessageBox(wxString(errorText.c_str(), wxConvUTF8), L"Error.", wxICON_ERROR);
	}
}


void CtrlDisassemblyView::onPopupClick(wxCommandEvent& evt)
{
	switch (evt.GetId())
	{
		case ID_DISASM_FOLLOWBRANCH:
			followBranch();
			break;
		case ID_DISASM_COPYADDRESS:
			if (wxTheClipboard->Open())
			{
				wchar_t text[64];
				swprintf(text, 64, L"%08X", curAddress);

				wxTheClipboard->SetData(new wxTextDataObject(text));
				wxTheClipboard->Close();
			}
			break;
		case ID_DISASM_GOTOADDRESS:
			postEvent(debEVT_GOTOADDRESS, 0);
			break;
		case ID_DISASM_GOTOINMEMORYVIEW:
			postEvent(debEVT_GOTOINMEMORYVIEW, curAddress);
			break;
		case ID_DISASM_COPYINSTRUCTIONHEX:
			copyInstructions(selectRangeStart, selectRangeEnd, false);
			break;
		case ID_DISASM_COPYINSTRUCTIONDISASM:
			copyInstructions(selectRangeStart, selectRangeEnd, true);
			break;
		case ID_DISASM_SETPCTOHERE:
			cpu->setPc(curAddress);
			redraw();
			break;
		case ID_DISASM_RUNTOHERE:
			postEvent(debEVT_RUNTOPOS, curAddress);
			break;
		case ID_DISASM_TOGGLEBREAKPOINT:
			toggleBreakpoint(false);
			break;
		case ID_DISASM_DISASSEMBLETOFILE:
			disassembleToFile();
			break;
		case ID_DISASM_RENAMEFUNCTION:
		{
			u32 funcBegin = cpu->GetSymbolMap().GetFunctionStart(curAddress);
			if (funcBegin != 0xFFFFFFFF)
			{
				wxString newName = wxGetTextFromUser(L"Enter the new function name", L"New function name",
					wxString(cpu->GetSymbolMap().GetLabelString(funcBegin).c_str(), wxConvUTF8), this);

				if (!newName.empty())
				{
					const wxCharBuffer converted = newName.ToUTF8();
					cpu->GetSymbolMap().SetLabelName(converted, funcBegin);
					postEvent(debEVT_MAPLOADED, 0);
					redraw();
				}
			}
			else
			{
				wxMessageBox(L"No symbol selected", L"Error", wxICON_ERROR);
			}
			break;
		}
		case ID_DISASM_REMOVEFUNCTION:
		{
			u32 funcBegin = cpu->GetSymbolMap().GetFunctionStart(curAddress);
			if (funcBegin != 0xFFFFFFFF)
			{
				u32 prevBegin = cpu->GetSymbolMap().GetFunctionStart(funcBegin - 1);
				if (prevBegin != 0xFFFFFFFF)
				{
					u32 expandedSize = cpu->GetSymbolMap().GetFunctionSize(prevBegin) + cpu->GetSymbolMap().GetFunctionSize(funcBegin);
					cpu->GetSymbolMap().SetFunctionSize(prevBegin, expandedSize);
				}

				cpu->GetSymbolMap().RemoveFunction(funcBegin, true);
				cpu->GetSymbolMap().SortSymbols();
				cpu->GetSymbolMap().UpdateActiveSymbols();
				manager.clear();

				postEvent(debEVT_MAPLOADED, 0);
			}
			else
			{
				postEvent(debEVT_SETSTATUSBARTEXT, L"WARNING: unable to find function symbol here");
			}

			redraw();
			break;
		}
		case ID_DISASM_ADDFUNCTION:
		{
			u32 prevBegin = cpu->GetSymbolMap().GetFunctionStart(curAddress);
			if (prevBegin != 0xFFFFFFFF)
			{
				if (prevBegin == curAddress)
				{
					postEvent(debEVT_SETSTATUSBARTEXT, L"WARNING: There's already a function entry point at this adress");
				}
				else
				{
					char symname[128];
					u32 prevSize = cpu->GetSymbolMap().GetFunctionSize(prevBegin);
					u32 newSize = curAddress - prevBegin;
					cpu->GetSymbolMap().SetFunctionSize(prevBegin, newSize);

					newSize = prevSize - newSize;
					sprintf(symname, "u_un_%08X", curAddress);
					cpu->GetSymbolMap().AddFunction(symname, curAddress, newSize);
					cpu->GetSymbolMap().SortSymbols();
					cpu->GetSymbolMap().UpdateActiveSymbols();
					manager.clear();

					postEvent(debEVT_MAPLOADED, 0);
				}
			}
			else
			{
				char symname[128];
				int newSize = selectRangeEnd - selectRangeStart;
				sprintf(symname, "u_un_%08X", selectRangeStart);
				cpu->GetSymbolMap().AddFunction(symname, selectRangeStart, newSize);
				cpu->GetSymbolMap().SortSymbols();
				cpu->GetSymbolMap().UpdateActiveSymbols();

				postEvent(debEVT_MAPLOADED, 0);
			}

			redraw();
			break;
		}
		case ID_DISASM_ASSEMBLE:
			assembleOpcode(curAddress, disassembleCurAddress());
			break;
		default:
			wxMessageBox(L"Unimplemented.", L"Unimplemented.", wxICON_INFORMATION);
			break;
	}
}

void CtrlDisassemblyView::keydownEvent(wxKeyEvent& evt)
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);

	switch (evt.GetKeyCode())
	{
		case 'g':
		case 'G':
		{
			u64 addr;
			if (!executeExpressionWindow(this, cpu, addr))
				return;
			gotoAddress(addr);
		}
		break;
		case 'd':
		case 'D':
			toggleBreakpoint(true);
			break;
		case 'e':
		case 'E':
			editBreakpoint();
			break;
		case 'b':
		case 'B':
		{
			BreakpointWindow bpw(this, cpu);
			if (bpw.ShowModal() == wxID_OK)
			{
				bpw.addBreakpoint();
				postEvent(debEVT_UPDATE, 0);
			}
		}
		break;
		case 'm':
		case 'M':
		{
			assembleOpcode(curAddress, "");
		}
		break;
		case WXK_LEFT:
			if (jumpStack.empty())
			{
				gotoAddress(cpu->getPC());
			}
			else
			{
				u32 addr = jumpStack[jumpStack.size() - 1];
				jumpStack.pop_back();
				gotoAddress(addr);
			}
			break;
		case WXK_RIGHT:
			followBranch();
			break;
		case WXK_UP:
			setCurAddress(manager.getNthPreviousAddress(curAddress, 1), wxGetKeyState(WXK_SHIFT));
			scrollAddressIntoView();
			scanFunctions();
			break;
		case WXK_DOWN:
			setCurAddress(manager.getNthNextAddress(curAddress, 1), wxGetKeyState(WXK_SHIFT));
			scrollAddressIntoView();
			scanFunctions();
			break;
		case WXK_TAB:
			displaySymbols = !displaySymbols;
			break;
		case WXK_PAGEUP:
			if (curAddress != windowStart && curAddressIsVisible())
			{
				setCurAddress(windowStart, wxGetKeyState(WXK_SHIFT));
				scrollAddressIntoView();
			}
			else
			{
				setCurAddress(manager.getNthPreviousAddress(windowStart, visibleRows), wxGetKeyState(WXK_SHIFT));
				scrollAddressIntoView();
			}
			scanFunctions();
			break;
		case WXK_PAGEDOWN:
			if (manager.getNthNextAddress(curAddress, 1) != windowEnd && curAddressIsVisible())
			{
				setCurAddress(manager.getNthPreviousAddress(windowEnd, 1), wxGetKeyState(WXK_SHIFT));
				scrollAddressIntoView();
			}
			else
			{
				setCurAddress(manager.getNthNextAddress(windowEnd, visibleRows - 1), wxGetKeyState(WXK_SHIFT));
				scrollAddressIntoView();
			}
			scanFunctions();
			break;
		case WXK_F8:
			postEvent(debEVT_STEPOUT, 0);
			break;
		case WXK_F10:
			postEvent(debEVT_STEPOVER, 0);
			return;
		case WXK_F11:
			if (evt.ShiftDown())
				postEvent(debEVT_STEPOUT, 0);
			else
				postEvent(debEVT_STEPINTO, 0);
			return;
		default:
			evt.Skip();
			break;
	}

	redraw();
}

void CtrlDisassemblyView::scrollbarEvent(wxScrollWinEvent& evt)
{
	int type = evt.GetEventType();
	if (type == wxEVT_SCROLLWIN_LINEUP)
	{
		windowStart = manager.getNthPreviousAddress(windowStart, 1);
		scanFunctions();
	}
	else if (type == wxEVT_SCROLLWIN_LINEDOWN)
	{
		windowStart = manager.getNthNextAddress(windowStart, 1);
		scanFunctions();
	}
	else if (type == wxEVT_SCROLLWIN_PAGEUP)
	{
		windowStart = manager.getNthPreviousAddress(windowStart, visibleRows);
		scanFunctions();
	}
	else if (type == wxEVT_SCROLLWIN_PAGEDOWN)
	{
		windowStart = manager.getNthNextAddress(windowStart, visibleRows);
		scanFunctions();
	}

	redraw();
}

void CtrlDisassemblyView::toggleBreakpoint(bool toggleEnabled)
{
	bool enabled;
	if (CBreakPoints::IsAddressBreakPoint(cpu->getCpuType(), curAddress, &enabled))
	{
		if (!enabled)
		{
			// enable disabled breakpoints
			CBreakPoints::ChangeBreakPoint(cpu->getCpuType(), curAddress, true);
		}
		else if (!toggleEnabled && CBreakPoints::GetBreakPointCondition(cpu->getCpuType(), curAddress) != NULL)
		{
			// don't just delete a breakpoint with a custom condition
			CBreakPoints::RemoveBreakPoint(cpu->getCpuType(), curAddress);
		}
		else if (toggleEnabled)
		{
			// disable breakpoint
			CBreakPoints::ChangeBreakPoint(cpu->getCpuType(), curAddress, false);
		}
		else
		{
			// otherwise just remove breakpoint
			CBreakPoints::RemoveBreakPoint(cpu->getCpuType(), curAddress);
		}
	}
	else
	{
		CBreakPoints::AddBreakPoint(cpu->getCpuType(), curAddress);
	}
}


void CtrlDisassemblyView::updateStatusBarText()
{
	char text[512];
	DisassemblyLineInfo line = DisassemblyLineInfo();
	manager.getLine(curAddress, true, line);

	text[0] = 0;
	if (line.type == DISTYPE_OPCODE || line.type == DISTYPE_MACRO)
	{
		if (line.info.isDataAccess)
		{
			if (!cpu->isValidAddress(line.info.dataAddress))
			{
				sprintf(text, "Invalid address %08X", line.info.dataAddress);
			}
			else if (line.info.lrType == MIPSAnalyst::LOADSTORE_NORMAL && line.info.dataAddress % line.info.dataSize)
			{
				sprintf(text, "Unaligned address %08X", line.info.dataAddress);
			}
			else
			{
				switch (line.info.dataSize)
				{
					case 1:
						sprintf(text, "[%08X] = %02X", line.info.dataAddress, cpu->read8(line.info.dataAddress));
						break;
					case 2:
						sprintf(text, "[%08X] = %04X", line.info.dataAddress, cpu->read16(line.info.dataAddress));
						break;
					case 4:
					{
						u32 data;
						if (line.info.lrType != MIPSAnalyst::LOADSTORE_NORMAL)
						{
							u32 address = line.info.dataAddress;
							data = cpu->read32(address & ~3) >> (address & 3) * 8;
							data |= cpu->read32((address + 3) & ~3) << (4 - (address & 3)) * 8;
						}
						else
						{
							data = cpu->read32(line.info.dataAddress);
						}

						const std::string addressSymbol = cpu->GetSymbolMap().GetLabelString(data);
						if (!addressSymbol.empty())
						{
							sprintf(text, "[%08X] = %s (%08X)", line.info.dataAddress, addressSymbol.c_str(), data);
						}
						else
						{
							sprintf(text, "[%08X] = %08X", line.info.dataAddress, data);
						}
						break;
					}
					case 8:
					{
						u64 data;
						if (line.info.lrType != MIPSAnalyst::LOADSTORE_NORMAL)
						{
							u32 address = line.info.dataAddress;
							data = cpu->read64(address & ~7) >> (address & 7) * 8;
							data |= cpu->read64((address + 7) & ~7) << (8 - (address & 7)) * 8;
						}
						else
						{
							data = cpu->read64(line.info.dataAddress);
						}

						sprintf(text, "[%08X] = %016" PRIX64, line.info.dataAddress, data);
						break;
					}
					case 16:
					{
						__aligned16 u128 data = cpu->read128(line.info.dataAddress);
						sprintf(text, "[%08X] = %016" PRIX64 "%016" PRIX64, line.info.dataAddress, data._u64[1], data._u64[0]);
						break;
					}
				}
			}
		}

		if (line.info.isBranch)
		{
			const std::string addressSymbol = cpu->GetSymbolMap().GetLabelString(line.info.branchTarget);
			if (addressSymbol.empty())
			{
				sprintf(text, "%08X", line.info.branchTarget);
			}
			else
			{
				sprintf(text, "%08X = %s", line.info.branchTarget, addressSymbol.c_str());
			}
		}
	}
	else if (line.type == DISTYPE_DATA)
	{
		u32 start = cpu->GetSymbolMap().GetDataStart(curAddress);
		if (start == 0xFFFFFFFF)
			start = curAddress;

		u32 diff = curAddress - start;
		const std::string label = cpu->GetSymbolMap().GetLabelString(start);

		if (!label.empty())
		{
			if (diff != 0)
				sprintf(text, "%08X (%s) + %08X", start, label.c_str(), diff);
			else
				sprintf(text, "%08X (%s)", start, label.c_str());
		}
		else
		{
			if (diff != 0)
				sprintf(text, "%08X + %08X", start, diff);
			else
				sprintf(text, "%08X", start);
		}
	}

	postEvent(debEVT_SETSTATUSBARTEXT, wxString(text, wxConvUTF8));
}

void CtrlDisassemblyView::mouseEvent(wxMouseEvent& evt)
{
	// left button
	wxEventType type = evt.GetEventType();
	bool hasFocus = wxWindow::FindFocus() == this;

	if (type == wxEVT_LEFT_DOWN || type == wxEVT_LEFT_DCLICK || type == wxEVT_RIGHT_DOWN)
	{
		u32 newAddress = yToAddress(evt.GetY());
		bool setNewAddress = true;

		if (type == wxEVT_RIGHT_DOWN && !wxGetKeyState(WXK_SHIFT))
		{
			// Maintain the current selection if right clicking into it.
			if (newAddress >= selectRangeStart && newAddress < selectRangeEnd)
				setNewAddress = false;
		}
		else
		{
			if (curAddress == newAddress && hasFocus)
				toggleBreakpoint(false);
		}

		if (setNewAddress)
			setCurAddress(newAddress, wxGetKeyState(WXK_SHIFT));

		SetFocus();
		SetFocusFromKbd();
	}
	else if (evt.GetEventType() == wxEVT_RIGHT_UP)
	{
		PopupMenu(&menu, evt.GetPosition());
		return;
	}
	else if (evt.GetEventType() == wxEVT_MOUSEWHEEL)
	{
		if (evt.GetWheelRotation() > 0)
		{
			windowStart = manager.getNthPreviousAddress(windowStart, 3);
			scanFunctions();
		}
		else if (evt.GetWheelRotation() < 0)
		{
			windowStart = manager.getNthNextAddress(windowStart, 3);
			scanFunctions();
		}
	}
	else if (evt.GetEventType() == wxEVT_MOTION)
	{
		if (evt.ButtonIsDown(wxMOUSE_BTN_LEFT))
		{
			int newAddress = yToAddress(evt.GetY());
			setCurAddress(newAddress, wxGetKeyState(WXK_SHIFT));
		}
		else
			return;
	}
	else
	{
		evt.Skip();
		return;
	}

	redraw();
}

void CtrlDisassemblyView::sizeEvent(wxSizeEvent& evt)
{
	wxSize s = evt.GetSize();
	visibleRows = s.GetWidth() / rowHeight;
}

u32 CtrlDisassemblyView::yToAddress(int y)
{
	int line = y / rowHeight;
	return manager.getNthNextAddress(windowStart, line);
}

bool CtrlDisassemblyView::curAddressIsVisible()
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);
	return curAddress >= windowStart && curAddress < windowEnd;
}

void CtrlDisassemblyView::scrollStepping(u32 newPc)
{
	u32 windowEnd = manager.getNthNextAddress(windowStart, visibleRows);

	newPc = manager.getStartAddress(newPc);
	if (newPc >= windowEnd || newPc >= manager.getNthPreviousAddress(windowEnd, 1))
	{
		windowStart = manager.getNthPreviousAddress(newPc, visibleRows - 2);
	}
}

std::string CtrlDisassemblyView::disassembleRange(u32 start, u32 size)
{
	std::string result;

	// gather all branch targets without labels
	std::set<u32> branchAddresses;
	for (u32 i = 0; i < size; i += 4)
	{
		MIPSAnalyst::MipsOpcodeInfo info = MIPSAnalyst::GetOpcodeInfo(cpu, start + i);

		if (info.isBranch && cpu->GetSymbolMap().GetLabelString(info.branchTarget).empty())
		{
			if (branchAddresses.find(info.branchTarget) == branchAddresses.end())
			{
				branchAddresses.insert(info.branchTarget);
			}
		}
	}

	u32 disAddress = start;
	bool previousLabel = true;
	DisassemblyLineInfo line = DisassemblyLineInfo();
	while (disAddress < start + size)
	{
		char addressText[64], buffer[512];

		manager.getLine(disAddress, displaySymbols, line);
		bool isLabel = getDisasmAddressText(disAddress, addressText, false, line.type == DISTYPE_OPCODE);

		if (isLabel)
		{
			if (!previousLabel)
				result += "\r\n";
			sprintf(buffer, "%s\r\n\r\n", addressText);
			result += buffer;
		}
		else if (branchAddresses.find(disAddress) != branchAddresses.end())
		{
			if (!previousLabel)
				result += "\r\n";
			sprintf(buffer, "pos_%08X:\r\n\r\n", disAddress);
			result += buffer;
		}

		if (line.info.isBranch && !line.info.isBranchToRegister && cpu->GetSymbolMap().GetLabelString(line.info.branchTarget).empty() && branchAddresses.find(line.info.branchTarget) != branchAddresses.end())
		{
			sprintf(buffer, "pos_%08X", line.info.branchTarget);
			line.params = line.params.substr(0, line.params.find("0x")) + buffer;
		}

		sprintf(buffer, "\t%s\t%s\r\n", line.name.c_str(), line.params.c_str());
		result += buffer;
		previousLabel = isLabel;
		disAddress += line.totalSize;
	}

	return result;
}

std::string CtrlDisassemblyView::disassembleCurAddress()
{
	DisassemblyLineInfo line = DisassemblyLineInfo();
	manager.getLine(curAddress, displaySymbols, line);
	return line.name + (line.params.length() > 0 ? " " + line.params : "");
}

void CtrlDisassemblyView::copyInstructions(u32 startAddr, u32 endAddr, bool withDisasm)
{
	if (!wxTheClipboard->Open())
	{
		wxMessageBox(L"Could not open clipboard.", L"Error", wxICON_ERROR);
		return;
	}

	if (!withDisasm)
	{
		int instructionSize = 4;
		int count = (endAddr - startAddr) / instructionSize;
		int space = count * 32;
		char* temp = new char[space];

		char* p = temp;
		for (u32 pos = startAddr; pos < endAddr; pos += instructionSize)
		{
			p += sprintf(p, "%08X", cpu->read32(pos));

			// Don't leave a trailing newline.
			if (pos + instructionSize < endAddr)
				p += sprintf(p, "\r\n");
		}

		wxTheClipboard->SetData(new wxTextDataObject(wxString(temp, wxConvUTF8)));
		delete[] temp;
	}
	else
	{
		std::string disassembly = disassembleRange(startAddr, endAddr - startAddr);
		wxTheClipboard->SetData(new wxTextDataObject(wxString(disassembly.c_str(), wxConvUTF8)));
	}

	wxTheClipboard->Close();
}

void CtrlDisassemblyView::disassembleToFile()
{
	wxFileDialog dlg(this, wxEmptyString, wxEmptyString, wxEmptyString, L"*.*", wxFD_SAVE);

	if (dlg.ShowModal() == wxID_CANCEL)
		return;

	std::string disassembly = disassembleRange(selectRangeStart, selectRangeEnd - selectRangeStart);
	wxFile output(dlg.GetPath(), wxFile::write);
	output.Write(wxString(disassembly.c_str(), wxConvUTF8));
}

void CtrlDisassemblyView::editBreakpoint()
{
	BreakpointWindow win(this, cpu);

	bool exists = false;
	if (CBreakPoints::IsAddressBreakPoint(cpu->getCpuType(), curAddress))
	{
		auto breakpoints = CBreakPoints::GetBreakpoints();
		for (size_t i = 0; i < breakpoints.size(); i++)
		{
			if (breakpoints[i].addr == curAddress)
			{
				win.loadFromBreakpoint(breakpoints[i]);
				exists = true;
				break;
			}
		}
	}

	if (!exists)
		win.initBreakpoint(curAddress);

	if (win.ShowModal() == wxID_OK)
	{
		if (exists)
			CBreakPoints::RemoveBreakPoint(cpu->getCpuType(), curAddress);
		win.addBreakpoint();
		postEvent(debEVT_UPDATE, 0);
	}
}

void CtrlDisassemblyView::getOpcodeText(u32 address, char* dest)
{
	DisassemblyLineInfo line = DisassemblyLineInfo();
	address = manager.getStartAddress(address);
	manager.getLine(address, displaySymbols, line);
	sprintf(dest, "%s  %s", line.name.c_str(), line.params.c_str());
}
