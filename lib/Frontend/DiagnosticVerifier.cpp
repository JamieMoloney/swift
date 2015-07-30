//===- DiagnosticVerifier.cpp - Diagnostic Verifier (-verify) -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the DiagnosticVerifier class.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/Lexer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
using namespace swift;

namespace {
  struct ExpectedFixIt {
    const char *FixitLoc;
    unsigned StartCol;
    unsigned EndCol;
    std::string Text;
  };

  struct ExpectedDiagnosticInfo {
    const char *Loc;
    llvm::SourceMgr::DiagKind Classification;
    
    // This is true if a '*' constraint is present to say that the diagnostic
    // may appear (or not) an uncounted number of times.
    bool mayAppear = false;

    // This is the raw input buffer for the message text, the part in the
    // {{...}}
    StringRef MessageRange;
    
    // This is the message string with escapes expanded.
    std::string MessageStr;
    unsigned LineNo = ~0U;
    
    std::vector<ExpectedFixIt> Fixits;

    ExpectedDiagnosticInfo(const char *Loc,
                           llvm::SourceMgr::DiagKind Classification)
      : Loc(Loc), Classification(Classification) {
    }
    
  };
}

static std::string getDiagKindString(llvm::SourceMgr::DiagKind Kind) {
  switch (Kind) {
  case llvm::SourceMgr::DK_Error: return "error";
  case llvm::SourceMgr::DK_Warning: return "warning";
  case llvm::SourceMgr::DK_Note: return "note";
  }
}



namespace {
  /// This class implements support for -verify mode in the compiler.  It
  /// buffers up diagnostics produced during compilation, then checks them
  /// against expected-error markers in the source file.
  class DiagnosticVerifier {
    SourceManager &SM;
    std::vector<llvm::SMDiagnostic> CapturedDiagnostics;
  public:
    explicit DiagnosticVerifier(SourceManager &SM) : SM(SM) {}

    void addDiagnostic(const llvm::SMDiagnostic &Diag) {
      CapturedDiagnostics.push_back(Diag);
    }

    /// verifyFile - After the file has been processed, check to see if we
    /// got all of the expected diagnostics and check to see if there were any
    /// unexpected ones.
    bool verifyFile(unsigned BufferID, bool autoApplyFixes);

    /// If there are any -verify errors (e.g. differences between expectations
    /// and actual diagnostics produced), apply fixits to the original source
    /// file and drop it back in place.
    void autoApplyFixes(unsigned BufferID,
                        ArrayRef<llvm::SMDiagnostic> diagnostics);
    
  private:
    std::vector<llvm::SMDiagnostic>::iterator
    findDiagnostic(const ExpectedDiagnosticInfo &Expected,
                   StringRef BufferName);

  };
} // end anonymous namespace




/// If we find the specified diagnostic in the list, return it.
/// Otherwise return CapturedDiagnostics.end().
std::vector<llvm::SMDiagnostic>::iterator
DiagnosticVerifier::findDiagnostic(const ExpectedDiagnosticInfo &Expected,
                                   StringRef BufferName) {
  for (auto I = CapturedDiagnostics.begin(), E = CapturedDiagnostics.end();
       I != E; ++I) {
    // Verify the file and line of the diagnostic.
    if (I->getLineNo() != (int)Expected.LineNo ||
        I->getFilename() != BufferName)
      continue;

    // Verify the classification and string.
    if (I->getKind() != Expected.Classification ||
        I->getMessage().find(Expected.MessageStr) == StringRef::npos)
      continue;

    // Okay, we found a match, hurray!
    return I;
  }

  return CapturedDiagnostics.end();
}

static unsigned getColumnNumber(StringRef buffer, llvm::SMLoc loc) {
  assert(loc.getPointer() >= buffer.data());
  assert((size_t)(loc.getPointer() - buffer.data()) <= buffer.size());

  StringRef UpToLoc = buffer.slice(0, loc.getPointer() - buffer.data());

  size_t ColumnNo = UpToLoc.size();
  size_t NewlinePos = UpToLoc.find_last_of("\r\n");
  if (NewlinePos != StringRef::npos)
    ColumnNo -= NewlinePos;

  return static_cast<unsigned>(ColumnNo);
}

/// Return true if the given \p ExpectedFixIt is in the fix-its emitted by
/// diagnostic \p D.
static bool checkForFixIt(const ExpectedFixIt &Expected,
                          const llvm::SMDiagnostic &D,
                          StringRef buffer) {
  for (auto &ActualFixIt : D.getFixIts()) {
    if (ActualFixIt.getText() != Expected.Text)
      continue;

    llvm::SMRange Range = ActualFixIt.getRange();
    if (getColumnNumber(buffer, Range.Start) != Expected.StartCol)
      continue;
    if (getColumnNumber(buffer, Range.End) != Expected.EndCol)
      continue;

    return true;
  }

  return false;
}

/// \brief After the file has been processed, check to see if we got all of
/// the expected diagnostics and check to see if there were any unexpected
/// ones.
bool DiagnosticVerifier::verifyFile(unsigned BufferID,
                                    bool shouldAutoApplyFixes) {
  const SourceLoc BufferStartLoc = SM.getLocForBufferStart(BufferID);
  CharSourceRange EntireRange = SM.getRangeForBuffer(BufferID);
  StringRef InputFile = SM.extractText(EntireRange);
  StringRef BufferName = SM.getIdentifierForBuffer(BufferID);

  // Queue up all of the diagnostics, allowing us to sort them and emit them in
  // file order.
  std::vector<llvm::SMDiagnostic> Errors;

  unsigned PrevExpectedContinuationLine = 0;

  std::vector<ExpectedDiagnosticInfo> ExpectedDiagnostics;
  
  auto addError = [&](const char *Loc, std::string message,
                      ArrayRef<llvm::SMFixIt> FixIts = {}) {
    auto loc = SourceLoc(llvm::SMLoc::getFromPointer(Loc));
    auto diag = SM.GetMessage(loc, llvm::SourceMgr::DK_Error, message,
                              {}, FixIts);
    Errors.push_back(diag);
  };
  
  
  // Scan the memory buffer looking for expected-note/warning/error.
  for (size_t Match = InputFile.find("expected-");
       Match != StringRef::npos; Match = InputFile.find("expected-", Match+1)) {
    // Process this potential match.  If we fail to process it, just move on to
    // the next match.
    StringRef MatchStart = InputFile.substr(Match);
    const char *DiagnosticLoc = MatchStart.data();

    llvm::SourceMgr::DiagKind ExpectedClassification;
    if (MatchStart.startswith("expected-note")) {
      ExpectedClassification = llvm::SourceMgr::DK_Note;
      MatchStart = MatchStart.substr(strlen("expected-note"));
    } else if (MatchStart.startswith("expected-warning")) {
      ExpectedClassification = llvm::SourceMgr::DK_Warning;
      MatchStart = MatchStart.substr(strlen("expected-warning"));
    } else if (MatchStart.startswith("expected-error")) {
      ExpectedClassification = llvm::SourceMgr::DK_Error;
      MatchStart = MatchStart.substr(strlen("expected-error"));
    } else
      continue;

    // Skip any whitespace before the {{.
    MatchStart = MatchStart.substr(MatchStart.find_first_not_of(" \t"));

    size_t TextStartIdx = MatchStart.find("{{");
    if (TextStartIdx == StringRef::npos) {
      addError(MatchStart.data(),
               "expected {{ in expected-warning/note/error line");
      continue;
    }

    int LineOffset = 0;
    if (TextStartIdx > 0 && MatchStart[0] == '@') {
      if (MatchStart[1] != '+' && MatchStart[1] != '-') {
        addError(MatchStart.data(), "expected '+'/'-' for line offset");
        continue;
      }
      StringRef Offs;
      if (MatchStart[1] == '+')
        Offs = MatchStart.slice(2, TextStartIdx).rtrim();
      else
        Offs = MatchStart.slice(1, TextStartIdx).rtrim();

      size_t SpaceIndex = Offs.find(' ');
      if (SpaceIndex != StringRef::npos && SpaceIndex < TextStartIdx) {
        size_t Delta = Offs.size() - SpaceIndex;
        MatchStart = MatchStart.substr(TextStartIdx - Delta);
        TextStartIdx = Delta;
        Offs = Offs.slice(0, SpaceIndex);
      } else {
        MatchStart = MatchStart.substr(TextStartIdx);
        TextStartIdx = 0;
      }

      if (Offs.getAsInteger(10, LineOffset)) {
        addError(MatchStart.data(), "expected line offset before '{{'");
        continue;
      }
    }

    ExpectedDiagnosticInfo Expected(DiagnosticLoc, ExpectedClassification);

    unsigned Count = 1;
    if (TextStartIdx > 0) {
      StringRef CountStr = MatchStart.substr(0, TextStartIdx).trim();
      if (CountStr == "*") {
        Expected.mayAppear = true;
      } else {
        if (CountStr.getAsInteger(10, Count)) {
          addError(MatchStart.data(), "expected match count before '{{'");
          continue;
        }
        if (Count == 0) {
          addError(MatchStart.data(),
                   "expected positive match count before '{{'");
          continue;
        }
      }

      // Resync up to the '{{'.
      MatchStart = MatchStart.substr(TextStartIdx);
    }

    size_t End = MatchStart.find("}}");
    if (End == StringRef::npos) {
      addError(MatchStart.data(),
          "didn't find '}}' to match '{{' in expected-warning/note/error line");
      continue;
    }

 
    llvm::SmallString<256> Buf;
    Expected.MessageRange = MatchStart.slice(2, End);
    Expected.MessageStr =
      Lexer::getEncodedStringSegment(Expected.MessageRange, Buf);
    if (PrevExpectedContinuationLine)
      Expected.LineNo = PrevExpectedContinuationLine;
    else
      Expected.LineNo = SM.getLineAndColumn(
          BufferStartLoc.getAdvancedLoc(MatchStart.data() - InputFile.data()),
          BufferID).first;
    Expected.LineNo += LineOffset;

    // Check if the next expected diagnostic should be in the same line.
    StringRef AfterEnd = MatchStart.substr(End + strlen("}}"));
    AfterEnd = AfterEnd.substr(AfterEnd.find_first_not_of(" \t"));
    if (AfterEnd.startswith("\\"))
      PrevExpectedContinuationLine = Expected.LineNo;
    else
      PrevExpectedContinuationLine = 0;

    
    // Scan for fix-its: {{10-14=replacement text}}
    StringRef ExtraChecks = MatchStart.substr(End+2).ltrim(" \t");
    while (ExtraChecks.startswith("{{")) {
      // First make sure we have a closing "}}".
      size_t EndLoc = ExtraChecks.find("}}");
      if (EndLoc == StringRef::npos) {
        addError(ExtraChecks.data(),
                 "didn't find '}}' to match '{{' in fix-it verification");
        break;
      }
      
      // Allow for close braces to appear in the replacement text.
      while (EndLoc+2 < ExtraChecks.size() && ExtraChecks[EndLoc+2] == '}')
        ++EndLoc;
      
      StringRef FixItStr = ExtraChecks.slice(2, EndLoc);
      // Check for matching a later "}}" on a different line.
      if (FixItStr.find_first_of("\r\n") != StringRef::npos) {
        addError(ExtraChecks.data(), "didn't find '}}' to match '{{' in "
                 "fix-it verification");
        break;
      }
      
      // Prepare for the next round of checks.
      ExtraChecks = ExtraChecks.substr(EndLoc+2).ltrim();
      
      // Parse the pieces of the fix-it.
      size_t MinusLoc = FixItStr.find('-');
      if (MinusLoc == StringRef::npos) {
        addError(FixItStr.data(), "expected '-' in fix-it verification");
        continue;
      }
      StringRef StartColStr = FixItStr.slice(0, MinusLoc);
      StringRef AfterMinus = FixItStr.substr(MinusLoc+1);
      
      size_t EqualLoc = AfterMinus.find('=');
      if (EqualLoc == StringRef::npos) {
        addError(AfterMinus.data(),
                 "expected '=' after '-' in fix-it verification");
        continue;
      }
      StringRef EndColStr = AfterMinus.slice(0, EqualLoc);
      StringRef AfterEqual = AfterMinus.substr(EqualLoc+1);
      
      ExpectedFixIt FixIt;
      FixIt.FixitLoc = StartColStr.data()-2;
      if (StartColStr.getAsInteger(10, FixIt.StartCol)) {
        addError(StartColStr.data(),
                 "invalid column number in fix-it verification");
        continue;
      }
      if (EndColStr.getAsInteger(10, FixIt.EndCol)) {
        addError(EndColStr.data(),
                 "invalid column number in fix-it verification");
        continue;
      }
      
      // Translate literal "\\n" into '\n', inefficiently.
      StringRef fixItText = AfterEqual.slice(0, EndLoc);
      for (const char *current = fixItText.begin(), *end = fixItText.end();
           current != end; /* in loop */) {
        if (*current == '\\' && current + 1 < end && *(current + 1) == 'n') {
          FixIt.Text += '\n';
          current += 2;
        } else {
          FixIt.Text += *current++;
        }
      }
      
      Expected.Fixits.push_back(FixIt);
    }

    // Add the diagnostic the expected number of times.
    for (; Count; --Count)
      ExpectedDiagnostics.push_back(Expected);
  }

  
  // Make sure all the expected diagnostics appeared.
  std::reverse(ExpectedDiagnostics.begin(), ExpectedDiagnostics.end());
  
  for (unsigned i = ExpectedDiagnostics.size(); i != 0; ) {
    --i;
    auto &expected = ExpectedDiagnostics[i];
    
    // Check to see if we had this expected diagnostic.
    auto FoundDiagnosticIter = findDiagnostic(expected, BufferName);
    if (FoundDiagnosticIter == CapturedDiagnostics.end()) {
      // Diagnostic didn't exist.  If this is a 'mayAppear' diagnostic, then
      // we're ok.  Otherwise, leave it in the list.
      if (expected.mayAppear)
        ExpectedDiagnostics.erase(ExpectedDiagnostics.begin()+i);
      continue;
    }
    
    auto &FoundDiagnostic = *FoundDiagnosticIter;

    // Verify that any expected fix-its are present in the diagnostic.
    for (auto fixit : expected.Fixits) {
      if (!checkForFixIt(fixit, FoundDiagnostic, InputFile)) {
        std::string Message;
        {
          llvm::raw_string_ostream OS(Message);
          OS << "expected fix-it not seen";
          
          if (!FoundDiagnostic.getFixIts().empty()) {
            OS << "; actual fix-its:";
            
            for (auto &ActualFixIt : FoundDiagnostic.getFixIts()) {
              llvm::SMRange Range = ActualFixIt.getRange();
              
              OS << " {{"
              << getColumnNumber(InputFile, Range.Start) << '-'
              << getColumnNumber(InputFile, Range.End) << '='
              << ActualFixIt.getText()
              << "}}";
            }
          }
        }
        
        addError(fixit.FixitLoc, std::move(Message));
      }
    }
    
    // Actually remove the diagnostic from the list, so we don't match it
    // again. We do have to do this after checking fix-its, though, because
    // the diagnostic owns its fix-its.
    CapturedDiagnostics.erase(FoundDiagnosticIter);
    
    // We found the diagnostic, so remove it... unless we allow an arbitrary
    // number of diagnostics, in which case we want to reprocess this.
    if (expected.mayAppear)
      ++i;
    else
      ExpectedDiagnostics.erase(ExpectedDiagnostics.begin()+i);
  }
  
  // Check to see if we have any incorrect diagnostics.  If so, diagnose them as
  // such.
  for (unsigned i = ExpectedDiagnostics.size(); i != 0; ) {
    --i;
    auto &expected = ExpectedDiagnostics[i];

    // Check to see if any found diagnostics have the right line and
    // classification, but the wrong text.
    auto I = CapturedDiagnostics.begin();
    for (auto E = CapturedDiagnostics.end(); I != E; ++I) {
      // Verify the file and line of the diagnostic.
      if (I->getLineNo() != (int)expected.LineNo ||
          I->getFilename() != BufferName ||
          I->getKind() != expected.Classification)
        continue;
      
      // Otherwise, we found it, break out.
      break;
    }

    if (I == CapturedDiagnostics.end()) continue;
    
    auto StartLoc = llvm::SMLoc::getFromPointer(expected.MessageRange.begin());
    auto EndLoc = llvm::SMLoc::getFromPointer(expected.MessageRange.end());
    
    llvm::SMFixIt fixIt(llvm::SMRange{ StartLoc, EndLoc }, I->getMessage());
    addError(expected.MessageRange.begin(), "incorrect message found", fixIt);
    CapturedDiagnostics.erase(I);
    ExpectedDiagnostics.erase(ExpectedDiagnostics.begin()+i);
  }
  
  

  // Diagnose expected diagnostics that didn't appear.
  std::reverse(ExpectedDiagnostics.begin(), ExpectedDiagnostics.end());
  for (auto const &expected : ExpectedDiagnostics) {
    std::string message = "expected "+getDiagKindString(expected.Classification)
      + " not produced";
    addError(expected.Loc, message);
  }
  
  // Verify that there are no diagnostics (in MemoryBuffer) left in the list.
  for (unsigned i = 0, e = CapturedDiagnostics.size(); i != e; ++i) {
    if (CapturedDiagnostics[i].getFilename() != BufferName)
      continue;

    std::string Message =
      "unexpected "+getDiagKindString(CapturedDiagnostics[i].getKind())+
      " produced: "+CapturedDiagnostics[i].getMessage().str();
    addError(CapturedDiagnostics[i].getLoc().getPointer(),
             Message);
  }

  // Sort the diagnostics by their address in the memory buffer as the primary
  // key.  This ensures that an "unexpected diagnostic" and
  // "expected diagnostic" in the same place are emitted next to each other.
  std::sort(Errors.begin(), Errors.end(),
            [&](const llvm::SMDiagnostic &lhs,
                const llvm::SMDiagnostic &rhs) -> bool {
              return lhs.getLoc().getPointer() < rhs.getLoc().getPointer();
            });

  // Emit all of the queue'd up errors.
  for (auto Err : Errors)
    SM.getLLVMSourceMgr().PrintMessage(llvm::errs(), Err);
  
  // If auto-apply fixits is on, rewrite the original source file.
  if (shouldAutoApplyFixes)
    autoApplyFixes(BufferID, Errors);
  
  return !Errors.empty();
}

/// If there are any -verify errors (e.g. differences between expectations
/// and actual diagnostics produced), apply fixits to the original source
/// file and drop it back in place.
void DiagnosticVerifier::autoApplyFixes(unsigned BufferID,
                                        ArrayRef<llvm::SMDiagnostic> diags) {
  // Walk the list of diagnostics, pulling out any fixits into a array of just
  // them.
  SmallVector<llvm::SMFixIt, 4> FixIts;
  for (auto &diag : diags)
    FixIts.append(diag.getFixIts().begin(), diag.getFixIts().end());

  // If we have no fixits to apply, avoid touching the file.
  if (FixIts.empty())
    return;
  
  // Sort the fixits by their start location.
  std::sort(FixIts.begin(), FixIts.end(),
            [&](const llvm::SMFixIt &lhs, const llvm::SMFixIt &rhs) -> bool {
              return lhs.getRange().Start.getPointer()
                   < rhs.getRange().Start.getPointer();
            });

  // Get the contents of the original source file.
  auto memBuffer = SM.getLLVMSourceMgr().getMemoryBuffer(BufferID);
  auto bufferRange = memBuffer->getBuffer();

  // Apply the fixes, building up a new buffer as an std::string.
  const char *LastPos = bufferRange.begin();
  std::string Result;
  
  for (auto &fix : FixIts) {
    // We cannot handle overlapping fixits, so assert that they don't happen.
    assert(LastPos <= fix.getRange().Start.getPointer() &&
           "Cannot handle overlapping fixits");
    
    // Keep anything from the last spot we've checked to the start of the fixit.
    Result.append(LastPos, fix.getRange().Start.getPointer());
    
    // Replace the content covered by the fixit with the replacement text.
    Result.append(fix.getText().begin(), fix.getText().end());
    
    // Next character to consider is at the end of the fixit.
    LastPos = fix.getRange().End.getPointer();
  }
  
  // Retain the end of the file.
  Result.append(LastPos, bufferRange.end());
  
  std::ofstream outs(memBuffer->getBufferIdentifier());
  outs << Result;
}

//===----------------------------------------------------------------------===//
// Main entrypoints
//===----------------------------------------------------------------------===//

/// VerifyModeDiagnosticHook - Every time a diagnostic is generated in -verify
/// mode, this function is called with the diagnostic.  We just buffer them up
/// until the end of the file.
static void VerifyModeDiagnosticHook(const llvm::SMDiagnostic &Diag,
                                     void *Context) {
  ((DiagnosticVerifier*)Context)->addDiagnostic(Diag);
}


/// enableDiagnosticVerifier - Set up the specified source manager so that
/// diagnostics are captured instead of being printed.
void swift::enableDiagnosticVerifier(SourceManager &SM) {
  SM.getLLVMSourceMgr().setDiagHandler(VerifyModeDiagnosticHook,
                                       new DiagnosticVerifier(SM));
}

/// verifyDiagnostics - Verify that captured diagnostics meet with the
/// expectations of the source files corresponding to the specified BufferIDs
/// and tear down our support for capturing and verifying diagnostics.
bool swift::verifyDiagnostics(SourceManager &SM, ArrayRef<unsigned> BufferIDs) {
  auto *Verifier = (DiagnosticVerifier*)SM.getLLVMSourceMgr().getDiagContext();
  SM.getLLVMSourceMgr().setDiagHandler(nullptr, nullptr);

  bool autoApplyFixes = true;
  
  bool HadError = false;

  for (auto &BufferID : BufferIDs)
    HadError |= Verifier->verifyFile(BufferID, autoApplyFixes);

  delete Verifier;

  return HadError;
}

