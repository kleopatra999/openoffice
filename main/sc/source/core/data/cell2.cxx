/**************************************************************
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * 
 *************************************************************/



// MARKER(update_precomp.py): autogen include statement, do not remove
#include "precompiled_sc.hxx"



// INCLUDE ---------------------------------------------------------------
#include <algorithm>
#include <deque>

#include <boost/bind.hpp>

#include <vcl/mapmod.hxx>
#include <editeng/editobj.hxx>
#include <editeng/editstat.hxx>

#include "cell.hxx"
#include "compiler.hxx"
#include "formula/errorcodes.hxx"
#include "document.hxx"
#include "rangenam.hxx"
#include "rechead.hxx"
#include "refupdat.hxx"
#include "scmatrix.hxx"
#include "editutil.hxx"
#include "chgtrack.hxx"
#include "externalrefmgr.hxx"

using namespace formula;

// STATIC DATA -----------------------------------------------------------

#ifdef USE_MEMPOOL
const sal_uInt16 nMemPoolEditCell = (0x1000 - 64) / sizeof(ScNoteCell);
IMPL_FIXEDMEMPOOL_NEWDEL( ScEditCell, nMemPoolEditCell, nMemPoolEditCell )
#endif

// ============================================================================

ScEditCell::ScEditCell( const EditTextObject* pObject, ScDocument* pDocP,
            const SfxItemPool* pFromPool )  :
        ScBaseCell( CELLTYPE_EDIT ),
        pString( NULL ),
        pDoc( pDocP )
{
    SetTextObject( pObject, pFromPool );
}

ScEditCell::ScEditCell( const ScEditCell& rCell, ScDocument& rDoc )  :
        ScBaseCell( rCell ),
        pString( NULL ),
        pDoc( &rDoc )
{
    SetTextObject( rCell.pData, rCell.pDoc->GetEditPool() );
}

ScEditCell::ScEditCell( const String& rString, ScDocument* pDocP )  :
        ScBaseCell( CELLTYPE_EDIT ),
        pString( NULL ),
        pDoc( pDocP )
{
    DBG_ASSERT( rString.Search('\n') != STRING_NOTFOUND ||
                rString.Search(CHAR_CR) != STRING_NOTFOUND,
                "EditCell mit einfachem Text !?!?" );

    EditEngine& rEngine = pDoc->GetEditEngine();
    rEngine.SetText( rString );
    pData = rEngine.CreateTextObject();
}

ScEditCell::~ScEditCell()
{
    delete pData;
    delete pString;

#ifdef DBG_UTIL
    eCellType = CELLTYPE_DESTROYED;
#endif
}

void ScEditCell::SetData( const EditTextObject* pObject,
            const SfxItemPool* pFromPool )
{
    if ( pString )
    {
        delete pString;
        pString = NULL;
    }
    delete pData;
    SetTextObject( pObject, pFromPool );
}

void ScEditCell::GetData( const EditTextObject*& rpObject ) const
{
    rpObject = pData;
}

void ScEditCell::GetString( String& rString ) const
{
    if ( pString )
        rString = *pString;
    else if ( pData )
    {
        // auch Text von URL-Feldern, Doc-Engine ist eine ScFieldEditEngine
        EditEngine& rEngine = pDoc->GetEditEngine();
        rEngine.SetText( *pData );
        rString = ScEditUtil::GetMultilineString(rEngine); // string with line separators between paragraphs
        // cache short strings for formulas
        if ( rString.Len() < 256 )
            ((ScEditCell*)this)->pString = new String( rString );   //! non-const
    }
    else
        rString.Erase();
}

void ScEditCell::SetTextObject( const EditTextObject* pObject,
            const SfxItemPool* pFromPool )
{
    if ( pObject )
    {
        if ( pFromPool && pDoc->GetEditPool() == pFromPool )
            pData = pObject->Clone();
        else
        {   //! anderer Pool
            // Leider gibt es keinen anderen Weg, um den Pool umzuhaengen,
            // als das Object durch eine entsprechende Engine zu schleusen..
            EditEngine& rEngine = pDoc->GetEditEngine();
            if ( pObject->HasOnlineSpellErrors() )
            {
                sal_uLong nControl = rEngine.GetControlWord();
                const sal_uLong nSpellControl = EE_CNTRL_ONLINESPELLING | EE_CNTRL_ALLOWBIGOBJS;
                sal_Bool bNewControl = ( (nControl & nSpellControl) != nSpellControl );
                if ( bNewControl )
                    rEngine.SetControlWord( nControl | nSpellControl );
                rEngine.SetText( *pObject );
                pData = rEngine.CreateTextObject();
                if ( bNewControl )
                    rEngine.SetControlWord( nControl );
            }
            else
            {
                rEngine.SetText( *pObject );
                pData = rEngine.CreateTextObject();
            }
        }
    }
    else
        pData = NULL;
}

// ============================================================================

namespace
{

using std::deque;

typedef SCCOLROW(*DimensionSelector)(const ScSingleRefData&);


static SCCOLROW lcl_GetCol(const ScSingleRefData& rData)
{
    return rData.nCol;
}


static SCCOLROW lcl_GetRow(const ScSingleRefData& rData)
{
    return rData.nRow;
}


static SCCOLROW lcl_GetTab(const ScSingleRefData& rData)
{
    return rData.nTab;
}


/** Check if both references span the same range in selected dimension.
 */
static bool
lcl_checkRangeDimension(
        const SingleDoubleRefProvider& rRef1,
        const SingleDoubleRefProvider& rRef2,
        const DimensionSelector aWhich)
{
    return
        aWhich(rRef1.Ref1) == aWhich(rRef2.Ref1)
        && aWhich(rRef1.Ref2) == aWhich(rRef2.Ref2);
}


static bool
lcl_checkRangeDimensions(
        const SingleDoubleRefProvider& rRef1,
        const SingleDoubleRefProvider& rRef2,
        bool& bCol, bool& bRow, bool& bTab)
{
    const bool bSameCols(lcl_checkRangeDimension(rRef1, rRef2, lcl_GetCol));
    const bool bSameRows(lcl_checkRangeDimension(rRef1, rRef2, lcl_GetRow));
    const bool bSameTabs(lcl_checkRangeDimension(rRef1, rRef2, lcl_GetTab));
        
    // Test if exactly two dimensions are equal
    if (!(bSameCols ^ bSameRows ^ bSameTabs)
            && (bSameCols || bSameRows || bSameTabs))
    {
        bCol = !bSameCols;
        bRow = !bSameRows;
        bTab = !bSameTabs;
        return true;
    }
    return false;
}


/** Check if references in given reference list can possibly
    form a range. To do that, two of their dimensions must be the same.
 */
static bool
lcl_checkRangeDimensions(
        const deque<ScToken*>::const_iterator aBegin,
        const deque<ScToken*>::const_iterator aEnd,
        bool& bCol, bool& bRow, bool& bTab)
{
    deque<ScToken*>::const_iterator aCur(aBegin);
    ++aCur;
    const SingleDoubleRefProvider aRef(**aBegin);
    bool bOk(false);
    {
        const SingleDoubleRefProvider aRefCur(**aCur);
        bOk = lcl_checkRangeDimensions(aRef, aRefCur, bCol, bRow, bTab);
    }
    while (bOk && aCur != aEnd)
    {
        const SingleDoubleRefProvider aRefCur(**aCur);
        bool bColTmp(false);
        bool bRowTmp(false);
        bool bTabTmp(false);
        bOk = lcl_checkRangeDimensions(aRef, aRefCur, bColTmp, bRowTmp, bTabTmp);
        bOk = bOk && (bCol == bColTmp && bRow == bRowTmp && bTab == bTabTmp);
        ++aCur;
    }

    if (bOk && aCur == aEnd)
    {
        bCol = bCol;
        bRow = bRow;
        bTab = bTab;
        return true;
    }
    return false;
}


bool
lcl_lessReferenceBy(
        const ScToken* const pRef1, const ScToken* const pRef2,
        const DimensionSelector aWhich)
{
    const SingleDoubleRefProvider rRef1(*pRef1);
    const SingleDoubleRefProvider rRef2(*pRef2);
    return aWhich(rRef1.Ref1) < aWhich(rRef2.Ref1);
}


/** Returns true if range denoted by token pRef2 starts immediately after
    range denoted by token pRef1. Dimension, in which the comparison takes
    place, is given by aWhich.
 */
bool
lcl_isImmediatelyFollowing(
        const ScToken* const pRef1, const ScToken* const pRef2,
        const DimensionSelector aWhich)
{
    const SingleDoubleRefProvider rRef1(*pRef1);
    const SingleDoubleRefProvider rRef2(*pRef2);
    return aWhich(rRef2.Ref1) - aWhich(rRef1.Ref2) == 1;
}


static bool
lcl_checkIfAdjacent(
        const deque<ScToken*>& rReferences,
        const DimensionSelector aWhich)
{
    typedef deque<ScToken*>::const_iterator Iter;
    Iter aBegin(rReferences.begin());
    Iter aEnd(rReferences.end());
    Iter aBegin1(aBegin);
    ++aBegin1, --aEnd;
    return std::equal(
            aBegin, aEnd, aBegin1,
            boost::bind(lcl_isImmediatelyFollowing, _1, _2, aWhich));
}


static void
lcl_fillRangeFromRefList(
        const deque<ScToken*>& rReferences, ScRange& rRange)
{
    const ScSingleRefData aStart(
            SingleDoubleRefProvider(*rReferences.front()).Ref1);
    rRange.aStart.Set(aStart.nCol, aStart.nRow, aStart.nTab);
    const ScSingleRefData aEnd(
            SingleDoubleRefProvider(*rReferences.back()).Ref2);
    rRange.aEnd.Set(aEnd.nCol, aEnd.nRow, aEnd.nTab);
}


static bool
lcl_refListFormsOneRange(
        const ScAddress& aPos, deque<ScToken*>& rReferences,
        ScRange& rRange)
{
    std::for_each(
            rReferences.begin(), rReferences.end(),
            bind(&ScToken::CalcAbsIfRel, _1, aPos))
        ;
    if (rReferences.size() == 1) {
        lcl_fillRangeFromRefList(rReferences, rRange);
        return true;
    }

    bool bCell(false);
    bool bRow(false);
    bool bTab(false);
    if (lcl_checkRangeDimensions(rReferences.begin(), rReferences.end(),
            bCell, bRow, bTab))
    {
        DimensionSelector aWhich;
        if (bCell)
        {
            aWhich = lcl_GetCol;
        }
        else if (bRow)
        {
            aWhich = lcl_GetRow;
        }
        else if (bTab)
        {
            aWhich = lcl_GetTab;
        }
        else
        {
            OSL_ENSURE(false, "lcl_checkRangeDimensions shouldn't allow that!");
            aWhich = lcl_GetRow;    // initialize to avoid warning
        }
        // Sort the references by start of range
        std::sort(rReferences.begin(), rReferences.end(),
                boost::bind(lcl_lessReferenceBy, _1, _2, aWhich));
        if (lcl_checkIfAdjacent(rReferences, aWhich))
        {
            lcl_fillRangeFromRefList(rReferences, rRange);
            return true;
        }
    }
    return false;
}


bool lcl_isReference(const FormulaToken& rToken)
{
    return
        rToken.GetType() == svSingleRef ||
        rToken.GetType() == svDoubleRef;
}

}

sal_Bool ScFormulaCell::IsEmpty()
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    return aResult.GetCellResultType() == formula::svEmptyCell;
}

sal_Bool ScFormulaCell::IsEmptyDisplayedAsString()
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    return aResult.IsEmptyDisplayedAsString();
}

sal_Bool ScFormulaCell::IsValue()
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    return aResult.IsValue();
}

double ScFormulaCell::GetValue()
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    if ((!pCode->GetCodeError() || pCode->GetCodeError() == errDoubleRef) &&
            !aResult.GetResultError())
        return aResult.GetDouble();
    return 0.0;
}

double ScFormulaCell::GetValueAlways()
{
    // for goal seek: return result value even if error code is set

    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    return aResult.GetDouble();
}

void ScFormulaCell::GetString( String& rString )
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    if ((!pCode->GetCodeError() || pCode->GetCodeError() == errDoubleRef) &&
            !aResult.GetResultError())
        rString = aResult.GetString();
    else
        rString.Erase();
}

const ScMatrix* ScFormulaCell::GetMatrix()
{
    if ( pDocument->GetAutoCalc() )
    {
        if( IsDirtyOrInTableOpDirty()
        // Was stored !bDirty but an accompanying matrix cell was bDirty?
        || (!bDirty && cMatrixFlag == MM_FORMULA && !aResult.GetMatrix().Is()))
            Interpret();
    }
    return aResult.GetMatrix();
}

sal_Bool ScFormulaCell::GetMatrixOrigin( ScAddress& rPos ) const
{
    switch ( cMatrixFlag )
    {
        case MM_FORMULA :
            rPos = aPos;
            return sal_True;
//        break;
        case MM_REFERENCE :
        {
            pCode->Reset();
            ScToken* t = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
            if( t )
            {
                ScSingleRefData& rRef = t->GetSingleRef();
                rRef.CalcAbsIfRel( aPos );
                if ( rRef.Valid() )
                {
                    rPos.Set( rRef.nCol, rRef.nRow, rRef.nTab );
                    return sal_True;
                }
            }
        }
        break;
    }
    return sal_False;
}


/*
 Edge-Values:

   8
 4   16
   2

 innerhalb: 1
 ausserhalb: 0
 (reserviert: offen: 32)
 */

sal_uInt16 ScFormulaCell::GetMatrixEdge( ScAddress& rOrgPos )
{
    switch ( cMatrixFlag )
    {
        case MM_FORMULA :
        case MM_REFERENCE :
        {
            static SCCOL nC;
            static SCROW nR;
            ScAddress aOrg;
            if ( !GetMatrixOrigin( aOrg ) )
                return 0;               // dumm gelaufen..
            if ( aOrg != rOrgPos )
            {   // erstes Mal oder andere Matrix als letztes Mal
                rOrgPos = aOrg;
                ScFormulaCell* pFCell;
                if ( cMatrixFlag == MM_REFERENCE )
                    pFCell = (ScFormulaCell*) pDocument->GetCell( aOrg );
                else
                    pFCell = this;      // this MM_FORMULA
                // this gibt's nur einmal, kein Vergleich auf pFCell==this
                if ( pFCell && pFCell->GetCellType() == CELLTYPE_FORMULA
                  && pFCell->cMatrixFlag == MM_FORMULA )
                {
                    pFCell->GetMatColsRows( nC, nR );
                    if ( nC == 0 || nR == 0 )
                    {   // aus altem Dokument geladen, neu erzeugen
                        nC = 1;
                        nR = 1;
                        ScAddress aTmpOrg;
                        ScBaseCell* pCell;
                        ScAddress aAdr( aOrg );
                        aAdr.IncCol();
                        sal_Bool bCont = sal_True;
                        do
                        {
                            pCell = pDocument->GetCell( aAdr );
                            if ( pCell && pCell->GetCellType() == CELLTYPE_FORMULA
                              && ((ScFormulaCell*)pCell)->cMatrixFlag == MM_REFERENCE
                              && GetMatrixOrigin( aTmpOrg ) && aTmpOrg == aOrg )
                            {
                                nC++;
                                aAdr.IncCol();
                            }
                            else
                                bCont = sal_False;
                        } while ( bCont );
                        aAdr = aOrg;
                        aAdr.IncRow();
                        bCont = sal_True;
                        do
                        {
                            pCell = pDocument->GetCell( aAdr );
                            if ( pCell && pCell->GetCellType() == CELLTYPE_FORMULA
                              && ((ScFormulaCell*)pCell)->cMatrixFlag == MM_REFERENCE
                              && GetMatrixOrigin( aTmpOrg ) && aTmpOrg == aOrg )
                            {
                                nR++;
                                aAdr.IncRow();
                            }
                            else
                                bCont = sal_False;
                        } while ( bCont );
                        pFCell->SetMatColsRows( nC, nR );
                    }
                }
                else
                {
#ifdef DBG_UTIL
                    String aTmp;
                    ByteString aMsg( "broken Matrix, no MatFormula at origin, Pos: " );
                    aPos.Format( aTmp, SCA_VALID_COL | SCA_VALID_ROW, pDocument );
                    aMsg += ByteString( aTmp, RTL_TEXTENCODING_ASCII_US );
                    aMsg += ", MatOrg: ";
                    aOrg.Format( aTmp, SCA_VALID_COL | SCA_VALID_ROW, pDocument );
                    aMsg += ByteString( aTmp, RTL_TEXTENCODING_ASCII_US );
                    DBG_ERRORFILE( aMsg.GetBuffer() );
#endif
                    return 0;           // bad luck ...
                }
            }
            // here we are, healthy and clean, somewhere in between
            SCsCOL dC = aPos.Col() - aOrg.Col();
            SCsROW dR = aPos.Row() - aOrg.Row();
            sal_uInt16 nEdges = 0;
            if ( dC >= 0 && dR >= 0 && dC < nC && dR < nR )
            {
                if ( dC == 0 )
                    nEdges |= 4;            // linke Kante
                if ( dC+1 == nC )
                    nEdges |= 16;           // rechte Kante
                if ( dR == 0 )
                    nEdges |= 8;            // obere Kante
                if ( dR+1 == nR )
                    nEdges |= 2;            // untere Kante
                if ( !nEdges )
                    nEdges = 1;             // mittendrin
            }
#ifdef DBG_UTIL
            else
            {
                String aTmp;
                ByteString aMsg( "broken Matrix, Pos: " );
                aPos.Format( aTmp, SCA_VALID_COL | SCA_VALID_ROW, pDocument );
                aMsg += ByteString( aTmp, RTL_TEXTENCODING_ASCII_US );
                aMsg += ", MatOrg: ";
                aOrg.Format( aTmp, SCA_VALID_COL | SCA_VALID_ROW, pDocument );
                aMsg += ByteString( aTmp, RTL_TEXTENCODING_ASCII_US );
                aMsg += ", MatCols: ";
                aMsg += ByteString::CreateFromInt32( nC );
                aMsg += ", MatRows: ";
                aMsg += ByteString::CreateFromInt32( nR );
                aMsg += ", DiffCols: ";
                aMsg += ByteString::CreateFromInt32( dC );
                aMsg += ", DiffRows: ";
                aMsg += ByteString::CreateFromInt32( dR );
                DBG_ERRORFILE( aMsg.GetBuffer() );
            }
#endif
            return nEdges;
//            break;
        }
        default:
            return 0;
    }
}

sal_uInt16 ScFormulaCell::GetErrCode()
{
    if (IsDirtyOrInTableOpDirty() && pDocument->GetAutoCalc())
        Interpret();
    /* FIXME: If ScTokenArray::SetCodeError() was really only for code errors
     * and not also abused for signaling other error conditions we could bail
     * out even before attempting to interpret broken code. */
    sal_uInt16 nErr =  pCode->GetCodeError();
    if (nErr)
        return nErr;
    return aResult.GetResultError();
}

sal_uInt16 ScFormulaCell::GetRawError()
{
    sal_uInt16 nErr =  pCode->GetCodeError();
    if (nErr)
        return nErr;
    return aResult.GetResultError();
}

sal_Bool ScFormulaCell::HasOneReference( ScRange& r ) const
{
    pCode->Reset();
    ScToken* p = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
    if( p && !pCode->GetNextReferenceRPN() )        // nur eine!
    {
        p->CalcAbsIfRel( aPos );
        SingleDoubleRefProvider aProv( *p );
        r.aStart.Set( aProv.Ref1.nCol,
                      aProv.Ref1.nRow,
                      aProv.Ref1.nTab );
        r.aEnd.Set( aProv.Ref2.nCol,
                    aProv.Ref2.nRow,
                    aProv.Ref2.nTab );
        return sal_True;
    }
    else
        return sal_False;
}

bool
ScFormulaCell::HasRefListExpressibleAsOneReference(ScRange& rRange) const
{
    /* If there appears just one reference in the formula, it's the same
       as HasOneReference(). If there are more of them, they can denote
       one range if they are (sole) arguments of one function.
       Union of these references must form one range and their
       intersection must be empty set.
    */

    // Detect the simple case of exactly one reference in advance without all 
    // overhead.
    // #i107741# Doing so actually makes outlines using SUBTOTAL(x;reference) 
    // work again, where the function does not have only references.
    if (HasOneReference( rRange))
        return true;

    pCode->Reset();
    // Get first reference, if any
    ScToken* const pFirstReference(
            dynamic_cast<ScToken*>(pCode->GetNextReferenceRPN()));
    if (pFirstReference)
    {
        // Collect all consecutive references, starting by the one
        // already found
        std::deque<ScToken*> aReferences;
        aReferences.push_back(pFirstReference);
        FormulaToken* pToken(pCode->NextRPN());
        FormulaToken* pFunction(0);
        while (pToken)
        {
            if (lcl_isReference(*pToken))
            {
                aReferences.push_back(dynamic_cast<ScToken*>(pToken));
                pToken = pCode->NextRPN();
            }
            else
            {
                if (pToken->IsFunction())
                {
                    pFunction = pToken;
                }
                break;
            }
        }
        if (pFunction && !pCode->GetNextReferenceRPN()
                && (pFunction->GetParamCount() == aReferences.size()))
        {
            return lcl_refListFormsOneRange(aPos, aReferences, rRange);
        }
    }
    return false;
}

sal_Bool ScFormulaCell::HasRelNameReference() const
{
    pCode->Reset();
    ScToken* t;
    while ( ( t = static_cast<ScToken*>(pCode->GetNextReferenceRPN()) ) != NULL )
    {
        if ( t->GetSingleRef().IsRelName() ||
                (t->GetType() == formula::svDoubleRef &&
                t->GetDoubleRef().Ref2.IsRelName()) )
            return sal_True;
    }
    return sal_False;
}

sal_Bool ScFormulaCell::HasColRowName() const
{
    pCode->Reset();
    return (pCode->GetNextColRowName() != NULL);
}

void ScFormulaCell::UpdateReference(UpdateRefMode eUpdateRefMode,
                                    const ScRange& r,
                                    SCsCOL nDx, SCsROW nDy, SCsTAB nDz,
                                    ScDocument* pUndoDoc, const ScAddress* pUndoCellPos )
{
    SCCOL nCol1 = r.aStart.Col();
    SCROW nRow1 = r.aStart.Row();
    SCTAB nTab1 = r.aStart.Tab();
    SCCOL nCol2 = r.aEnd.Col();
    SCROW nRow2 = r.aEnd.Row();
    SCTAB nTab2 = r.aEnd.Tab();
    SCCOL nCol = aPos.Col();
    SCROW nRow = aPos.Row();
    SCTAB nTab = aPos.Tab();
    ScAddress aUndoPos( aPos );         // position for undo cell in pUndoDoc
    if ( pUndoCellPos )
        aUndoPos = *pUndoCellPos;
    ScAddress aOldPos( aPos );
//  sal_Bool bPosChanged = sal_False;           // ob diese Zelle bewegt wurde
    sal_Bool bIsInsert = sal_False;
    if (eUpdateRefMode == URM_INSDEL)
    {
        bIsInsert = (nDx >= 0 && nDy >= 0 && nDz >= 0);
        if ( nDx && nRow >= nRow1 && nRow <= nRow2 &&
            nTab >= nTab1 && nTab <= nTab2 )
        {
            if (nCol >= nCol1)
            {
                nCol = sal::static_int_cast<SCCOL>( nCol + nDx );
                if ((SCsCOL) nCol < 0)
                    nCol = 0;
                else if ( nCol > MAXCOL )
                    nCol = MAXCOL;
                aPos.SetCol( nCol );
//              bPosChanged = sal_True;
            }
        }
        if ( nDy && nCol >= nCol1 && nCol <= nCol2 &&
            nTab >= nTab1 && nTab <= nTab2 )
        {
            if (nRow >= nRow1)
            {
                nRow = sal::static_int_cast<SCROW>( nRow + nDy );
                if ((SCsROW) nRow < 0)
                    nRow = 0;
                else if ( nRow > MAXROW )
                    nRow = MAXROW;
                aPos.SetRow( nRow );
//              bPosChanged = sal_True;
            }
        }
        if ( nDz && nCol >= nCol1 && nCol <= nCol2 &&
            nRow >= nRow1 && nRow <= nRow2 )
        {
            if (nTab >= nTab1)
            {
                SCTAB nMaxTab = pDocument->GetTableCount() - 1;
                nTab = sal::static_int_cast<SCTAB>( nTab + nDz );
                if ((SCsTAB) nTab < 0)
                    nTab = 0;
                else if ( nTab > nMaxTab )
                    nTab = nMaxTab;
                aPos.SetTab( nTab );
//              bPosChanged = sal_True;
            }
        }
    }
    else if ( r.In( aPos ) )
    {
        aOldPos.Set( nCol - nDx, nRow - nDy, nTab - nDz );
//      bPosChanged = sal_True;
    }

    sal_Bool bHasRefs = sal_False;
    sal_Bool bHasColRowNames = sal_False;
    sal_Bool bOnRefMove = sal_False;
    if ( !pDocument->IsClipOrUndo() )
    {
        pCode->Reset();
        bHasRefs = (pCode->GetNextReferenceRPN() != NULL);
        if ( !bHasRefs || eUpdateRefMode == URM_COPY )
        {
            pCode->Reset();
            bHasColRowNames = (pCode->GetNextColRowName() != NULL);
            bHasRefs = bHasRefs || bHasColRowNames;
        }
        bOnRefMove = pCode->IsRecalcModeOnRefMove();
    }
    if( bHasRefs || bOnRefMove )
    {
        ScTokenArray* pOld = pUndoDoc ? pCode->Clone() : NULL;
        sal_Bool bValChanged;
        ScRangeData* pRangeData;
        sal_Bool bRangeModified;            // any range, not only shared formula
        sal_Bool bRefSizeChanged;
        if ( bHasRefs )
        {
            ScCompiler aComp(pDocument, aPos, *pCode);
            aComp.SetGrammar(pDocument->GetGrammar());
            pRangeData = aComp.UpdateReference(eUpdateRefMode, aOldPos, r,
                                             nDx, nDy, nDz,
                                             bValChanged, bRefSizeChanged);
            bRangeModified = aComp.HasModifiedRange();
        }
        else
        {
            bValChanged = sal_False;
            pRangeData = NULL;
            bRangeModified = sal_False;
            bRefSizeChanged = sal_False;
        }
        if ( bOnRefMove )
            bOnRefMove = (bValChanged || (aPos != aOldPos));
            // Cell may reference itself, e.g. ocColumn, ocRow without parameter

        sal_Bool bColRowNameCompile, bHasRelName, bNewListening, bInDeleteUndo;
        if ( bHasRefs )
        {
            // Upon Insert ColRowNames have to be recompiled in case the
            // insertion occurs right in front of the range.
            bColRowNameCompile =
                (eUpdateRefMode == URM_INSDEL && (nDx > 0 || nDy > 0));
            if ( bColRowNameCompile )
            {
                bColRowNameCompile = sal_False;
                ScToken* t;
                ScRangePairList* pColList = pDocument->GetColNameRanges();
                ScRangePairList* pRowList = pDocument->GetRowNameRanges();
                pCode->Reset();
                while ( !bColRowNameCompile && (t = static_cast<ScToken*>(pCode->GetNextColRowName())) != NULL )
                {
                    ScSingleRefData& rRef = t->GetSingleRef();
                    if ( nDy > 0 && rRef.IsColRel() )
                    {   // ColName
                        rRef.CalcAbsIfRel( aPos );
                        ScAddress aAdr( rRef.nCol, rRef.nRow, rRef.nTab );
                        ScRangePair* pR = pColList->Find( aAdr );
                        if ( pR )
                        {   // definiert
                            if ( pR->GetRange(1).aStart.Row() == nRow1 )
                                bColRowNameCompile = sal_True;
                        }
                        else
                        {   // on the fly
                            if ( rRef.nRow + 1 == nRow1 )
                                bColRowNameCompile = sal_True;
                        }
                    }
                    if ( nDx > 0 && rRef.IsRowRel() )
                    {   // RowName
                        rRef.CalcAbsIfRel( aPos );
                        ScAddress aAdr( rRef.nCol, rRef.nRow, rRef.nTab );
                        ScRangePair* pR = pRowList->Find( aAdr );
                        if ( pR )
                        {   // definiert
                            if ( pR->GetRange(1).aStart.Col() == nCol1 )
                                bColRowNameCompile = sal_True;
                        }
                        else
                        {   // on the fly
                            if ( rRef.nCol + 1 == nCol1 )
                                bColRowNameCompile = sal_True;
                        }
                    }
                }
            }
            else if ( eUpdateRefMode == URM_MOVE )
            {   // bei Move/D&D neu kompilieren wenn ColRowName verschoben wurde
                // oder diese Zelle auf einen zeigt und verschoben wurde
                bColRowNameCompile = bCompile;      // evtl. aus Copy-ctor
                if ( !bColRowNameCompile )
                {
                    sal_Bool bMoved = (aPos != aOldPos);
                    pCode->Reset();
                    ScToken* t = static_cast<ScToken*>(pCode->GetNextColRowName());
                    if ( t && bMoved )
                        bColRowNameCompile = sal_True;
                    while ( t && !bColRowNameCompile )
                    {
                        ScSingleRefData& rRef = t->GetSingleRef();
                        rRef.CalcAbsIfRel( aPos );
                        if ( rRef.Valid() )
                        {
                            ScAddress aAdr( rRef.nCol, rRef.nRow, rRef.nTab );
                            if ( r.In( aAdr ) )
                                bColRowNameCompile = sal_True;
                        }
                        t = static_cast<ScToken*>(pCode->GetNextColRowName());
                    }
                }
            }
            else if ( eUpdateRefMode == URM_COPY && bHasColRowNames && bValChanged )
            {
                bColRowNameCompile = sal_True;
            }
            ScChangeTrack* pChangeTrack = pDocument->GetChangeTrack();
            if ( pChangeTrack && pChangeTrack->IsInDeleteUndo() )
                bInDeleteUndo = sal_True;
            else
                bInDeleteUndo = sal_False;
            // RelNameRefs are always moved
            bHasRelName = HasRelNameReference();
            // Reference changed and new listening needed?
            // Except in Insert/Delete without specialties.
            bNewListening = (bRangeModified || pRangeData || bColRowNameCompile
                    || (bValChanged && (eUpdateRefMode != URM_INSDEL ||
                            bInDeleteUndo || bRefSizeChanged)) ||
                    (bHasRelName && eUpdateRefMode != URM_COPY))
                // #i36299# Don't duplicate action during cut&paste / drag&drop
                // on a cell in the range moved, start/end listeners is done
                // via ScDocument::DeleteArea() and ScDocument::CopyFromClip().
                && !(eUpdateRefMode == URM_MOVE &&
                        pDocument->IsInsertingFromOtherDoc() && r.In(aPos));
            if ( bNewListening )
                EndListeningTo( pDocument, pOld, aOldPos );
        }
        else
        {
            bColRowNameCompile = bHasRelName = bNewListening = bInDeleteUndo =
                sal_False;
        }

        sal_Bool bNeedDirty;
        // NeedDirty bei Aenderungen ausser Copy und Move/Insert ohne RelNames
        if ( bRangeModified || pRangeData || bColRowNameCompile ||
                (bValChanged && eUpdateRefMode != URM_COPY &&
                 (eUpdateRefMode != URM_MOVE || bHasRelName) &&
                 (!bIsInsert || bHasRelName || bInDeleteUndo ||
                  bRefSizeChanged)) || bOnRefMove)
            bNeedDirty = sal_True;
        else
            bNeedDirty = sal_False;
        if (pUndoDoc && (bValChanged || pRangeData || bOnRefMove))
        {
            //  Copy the cell to aUndoPos, which is its current position in the document,
            //  so this works when UpdateReference is called before moving the cells
            //  (InsertCells/DeleteCells - aPos is changed above) as well as when UpdateReference
            //  is called after moving the cells (MoveBlock/PasteFromClip - aOldPos is changed).

            // If there is already a formula cell in the undo document, don't overwrite it,
            // the first (oldest) is the important cell.
            if ( pUndoDoc->GetCellType( aUndoPos ) != CELLTYPE_FORMULA )
            {
                ScFormulaCell* pFCell = new ScFormulaCell( pUndoDoc, aUndoPos,
                        pOld, eTempGrammar, cMatrixFlag );
                pFCell->aResult.SetToken( NULL);  // to recognize it as changed later (Cut/Paste!)
                pUndoDoc->PutCell( aUndoPos, pFCell );
            }
        }
        // #i116833# If the formula is changed, always invalidate the stream (even if the result is the same).
        // If the formula is moved, the change is recognized separately.
        if (bValChanged && pDocument->IsStreamValid(aPos.Tab()))
            pDocument->SetStreamValid(aPos.Tab(), sal_False);
        bValChanged = sal_False;
        if ( pRangeData )
        {   // Replace shared formula with own formula
            pDocument->RemoveFromFormulaTree( this );   // update formula count
            delete pCode;
            pCode = pRangeData->GetCode()->Clone();
            // #i18937# #i110008# call MoveRelWrap, but with the old position
            ScCompiler::MoveRelWrap(*pCode, pDocument, aOldPos, pRangeData->GetMaxCol(), pRangeData->GetMaxRow());
            ScCompiler aComp2(pDocument, aPos, *pCode);
            aComp2.SetGrammar(pDocument->GetGrammar());
            aComp2.UpdateSharedFormulaReference( eUpdateRefMode, aOldPos, r,
                nDx, nDy, nDz );
            bValChanged = sal_True;
            bNeedDirty = sal_True;
        }
        if ( ( bCompile = (bCompile || bValChanged || bRangeModified || bColRowNameCompile) ) != 0 )
        {
            CompileTokenArray( bNewListening ); // kein Listening
            bNeedDirty = sal_True;
        }
        if ( !bInDeleteUndo )
        {   // In ChangeTrack Delete-Reject listeners are established in
            // InsertCol/InsertRow
            if ( bNewListening )
            {
                if ( eUpdateRefMode == URM_INSDEL )
                {
                    // Inserts/Deletes re-establish listeners after all
                    // UpdateReference calls.
                    // All replaced shared formula listeners have to be
                    // established after an Insert or Delete. Do nothing here.
                    SetNeedsListening( sal_True);
                }
                else
                    StartListeningTo( pDocument );
            }
        }
        if ( bNeedDirty && (!(eUpdateRefMode == URM_INSDEL && bHasRelName) || pRangeData) )
        {   // Referenzen abgeschnitten, ungueltig o.ae.?
            sal_Bool bOldAutoCalc = pDocument->GetAutoCalc();
            // kein Interpret in SubMinimalRecalc wegen evtl. falscher Referenzen
            pDocument->SetAutoCalc( sal_False );
            SetDirty();
            pDocument->SetAutoCalc( bOldAutoCalc );
        }

        delete pOld;
    }
}

void ScFormulaCell::UpdateInsertTab(SCTAB nTable)
{
    sal_Bool bPosChanged = ( aPos.Tab() >= nTable ? sal_True : sal_False );
    pCode->Reset();
    if( pCode->GetNextReferenceRPN() && !pDocument->IsClipOrUndo() )
    {
        EndListeningTo( pDocument );
        // IncTab _nach_ EndListeningTo und _vor_ Compiler UpdateInsertTab !
        if ( bPosChanged )
            aPos.IncTab();
        ScRangeData* pRangeData;
        ScCompiler aComp(pDocument, aPos, *pCode);
        aComp.SetGrammar(pDocument->GetGrammar());
        pRangeData = aComp.UpdateInsertTab( nTable, sal_False );
        if (pRangeData)                     // Shared Formula gegen echte Formel
        {                                   // austauschen
            sal_Bool bRefChanged;
            pDocument->RemoveFromFormulaTree( this );   // update formula count
            delete pCode;
            pCode = new ScTokenArray( *pRangeData->GetCode() );
            ScCompiler aComp2(pDocument, aPos, *pCode);
            aComp2.SetGrammar(pDocument->GetGrammar());
            aComp2.MoveRelWrap(pRangeData->GetMaxCol(), pRangeData->GetMaxRow());
            aComp2.UpdateInsertTab( nTable, sal_False );
            // If the shared formula contained a named range/formula containing
            // an absolute reference to a sheet, those have to be readjusted.
            aComp2.UpdateDeleteTab( nTable, sal_False, sal_True, bRefChanged );
            bCompile = sal_True;
        }
        // kein StartListeningTo weil pTab[nTab] noch nicht existiert!
    }
    else if ( bPosChanged )
        aPos.IncTab();
}

sal_Bool ScFormulaCell::UpdateDeleteTab(SCTAB nTable, sal_Bool bIsMove)
{
    sal_Bool bRefChanged = sal_False;
    sal_Bool bPosChanged = ( aPos.Tab() > nTable ? sal_True : sal_False );
    pCode->Reset();
    if( pCode->GetNextReferenceRPN() && !pDocument->IsClipOrUndo() )
    {
        EndListeningTo( pDocument );
        // IncTab _nach_ EndListeningTo und _vor_ Compiler UpdateDeleteTab !
        if ( bPosChanged )
            aPos.IncTab(-1);
        ScRangeData* pRangeData;
        ScCompiler aComp(pDocument, aPos, *pCode);
        aComp.SetGrammar(pDocument->GetGrammar());
        pRangeData = aComp.UpdateDeleteTab(nTable, bIsMove, sal_False, bRefChanged);
        if (pRangeData)                     // Shared Formula gegen echte Formel
        {                                   // austauschen
            pDocument->RemoveFromFormulaTree( this );   // update formula count
            delete pCode;
            pCode = pRangeData->GetCode()->Clone();
            ScCompiler aComp2(pDocument, aPos, *pCode);
            aComp2.SetGrammar(pDocument->GetGrammar());
            aComp2.CompileTokenArray();
            aComp2.MoveRelWrap(pRangeData->GetMaxCol(), pRangeData->GetMaxRow());
            aComp2.UpdateDeleteTab( nTable, sal_False, sal_False, bRefChanged );
            // If the shared formula contained a named range/formula containing
            // an absolute reference to a sheet, those have to be readjusted.
            aComp2.UpdateInsertTab( nTable,sal_True );
            // bRefChanged kann beim letzten UpdateDeleteTab zurueckgesetzt worden sein
            bRefChanged = sal_True;
            bCompile = sal_True;
        }
        // kein StartListeningTo weil pTab[nTab] noch nicht korrekt!
    }
    else if ( bPosChanged )
        aPos.IncTab(-1);

    return bRefChanged;
}

void ScFormulaCell::UpdateMoveTab( SCTAB nOldPos, SCTAB nNewPos, SCTAB nTabNo )
{
    pCode->Reset();
    if( pCode->GetNextReferenceRPN() && !pDocument->IsClipOrUndo() )
    {
        EndListeningTo( pDocument );
        // SetTab _nach_ EndListeningTo und _vor_ Compiler UpdateMoveTab !
        aPos.SetTab( nTabNo );
        ScRangeData* pRangeData;
        ScCompiler aComp(pDocument, aPos, *pCode);
        aComp.SetGrammar(pDocument->GetGrammar());
        pRangeData = aComp.UpdateMoveTab( nOldPos, nNewPos, sal_False );
        if (pRangeData)                     // Shared Formula gegen echte Formel
        {                                   // austauschen
            pDocument->RemoveFromFormulaTree( this );   // update formula count
            delete pCode;
            pCode = pRangeData->GetCode()->Clone();
            ScCompiler aComp2(pDocument, aPos, *pCode);
            aComp2.SetGrammar(pDocument->GetGrammar());
            aComp2.CompileTokenArray();
            aComp2.MoveRelWrap(pRangeData->GetMaxCol(), pRangeData->GetMaxRow());
            aComp2.UpdateMoveTab( nOldPos, nNewPos, sal_True );
            bCompile = sal_True;
        }
        // kein StartListeningTo weil pTab[nTab] noch nicht korrekt!
    }
    else
        aPos.SetTab( nTabNo );
}

void ScFormulaCell::UpdateInsertTabAbs(SCTAB nTable)
{
    if( !pDocument->IsClipOrUndo() )
    {
        pCode->Reset();
        ScToken* p = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
        while( p )
        {
            ScSingleRefData& rRef1 = p->GetSingleRef();
            if( !rRef1.IsTabRel() && (SCsTAB) nTable <= rRef1.nTab )
                rRef1.nTab++;
            if( p->GetType() == formula::svDoubleRef )
            {
                ScSingleRefData& rRef2 = p->GetDoubleRef().Ref2;
                if( !rRef2.IsTabRel() && (SCsTAB) nTable <= rRef2.nTab )
                    rRef2.nTab++;
            }
            p = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
        }
    }
}

sal_Bool ScFormulaCell::TestTabRefAbs(SCTAB nTable)
{
    sal_Bool bRet = sal_False;
    if( !pDocument->IsClipOrUndo() )
    {
        pCode->Reset();
        ScToken* p = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
        while( p )
        {
            ScSingleRefData& rRef1 = p->GetSingleRef();
            if( !rRef1.IsTabRel() )
            {
                if( (SCsTAB) nTable != rRef1.nTab )
                    bRet = sal_True;
                else if (nTable != aPos.Tab())
                    rRef1.nTab = aPos.Tab();
            }
            if( p->GetType() == formula::svDoubleRef )
            {
                ScSingleRefData& rRef2 = p->GetDoubleRef().Ref2;
                if( !rRef2.IsTabRel() )
                {
                    if( (SCsTAB) nTable != rRef2.nTab )
                        bRet = sal_True;
                    else if (nTable != aPos.Tab())
                        rRef2.nTab = aPos.Tab();
                }
            }
            p = static_cast<ScToken*>(pCode->GetNextReferenceRPN());
        }
    }
    return bRet;
}

void ScFormulaCell::UpdateCompile( sal_Bool bForceIfNameInUse )
{
    if ( bForceIfNameInUse && !bCompile )
        bCompile = pCode->HasNameOrColRowName();
    if ( bCompile )
        pCode->SetCodeError( 0 );   // make sure it will really be compiled
    CompileTokenArray();
}

//  Referenzen transponieren - wird nur in Clipboard-Dokumenten aufgerufen

void ScFormulaCell::TransposeReference()
{
    sal_Bool bFound = sal_False;
    pCode->Reset();
    ScToken* t;
    while ( ( t = static_cast<ScToken*>(pCode->GetNextReference()) ) != NULL )
    {
        ScSingleRefData& rRef1 = t->GetSingleRef();
        if ( rRef1.IsColRel() && rRef1.IsRowRel() )
        {
            sal_Bool bDouble = (t->GetType() == formula::svDoubleRef);
            ScSingleRefData& rRef2 = (bDouble ? t->GetDoubleRef().Ref2 : rRef1);
            if ( !bDouble || (rRef2.IsColRel() && rRef2.IsRowRel()) )
            {
                sal_Int16 nTemp;

                nTemp = rRef1.nRelCol;
                rRef1.nRelCol = static_cast<SCCOL>(rRef1.nRelRow);
                rRef1.nRelRow = static_cast<SCROW>(nTemp);

                if ( bDouble )
                {
                    nTemp = rRef2.nRelCol;
                    rRef2.nRelCol = static_cast<SCCOL>(rRef2.nRelRow);
                    rRef2.nRelRow = static_cast<SCROW>(nTemp);
                }

                bFound = sal_True;
            }
        }
    }

    if (bFound)
        bCompile = sal_True;
}

void ScFormulaCell::UpdateTranspose( const ScRange& rSource, const ScAddress& rDest,
                                        ScDocument* pUndoDoc )
{
    EndListeningTo( pDocument );

    ScAddress aOldPos = aPos;
    sal_Bool bPosChanged = sal_False;           // ob diese Zelle bewegt wurde

    ScRange aDestRange( rDest, ScAddress(
                static_cast<SCCOL>(rDest.Col() + rSource.aEnd.Row() - rSource.aStart.Row()),
                static_cast<SCROW>(rDest.Row() + rSource.aEnd.Col() - rSource.aStart.Col()),
                rDest.Tab() + rSource.aEnd.Tab() - rSource.aStart.Tab() ) );
    if ( aDestRange.In( aOldPos ) )
    {
        //  Position zurueckrechnen
        SCsCOL nRelPosX = aOldPos.Col();
        SCsROW nRelPosY = aOldPos.Row();
        SCsTAB nRelPosZ = aOldPos.Tab();
        ScRefUpdate::DoTranspose( nRelPosX, nRelPosY, nRelPosZ, pDocument, aDestRange, rSource.aStart );
        aOldPos.Set( nRelPosX, nRelPosY, nRelPosZ );
        bPosChanged = sal_True;
    }

    ScTokenArray* pOld = pUndoDoc ? pCode->Clone() : NULL;
    sal_Bool bRefChanged = sal_False;
    ScToken* t;

    ScRangeData* pShared = NULL;
    pCode->Reset();
    while( (t = static_cast<ScToken*>(pCode->GetNextReferenceOrName())) != NULL )
    {
        if( t->GetOpCode() == ocName )
        {
            ScRangeData* pName = pDocument->GetRangeName()->FindIndex( t->GetIndex() );
            if (pName)
            {
                if (pName->IsModified())
                    bRefChanged = sal_True;
                if (pName->HasType(RT_SHAREDMOD))
                    pShared = pName;
            }
        }
        else if( t->GetType() != svIndex )
        {
            t->CalcAbsIfRel( aOldPos );
            sal_Bool bMod;
            {   // own scope for SingleDoubleRefModifier dtor if SingleRef
                SingleDoubleRefModifier aMod( *t );
                ScComplexRefData& rRef = aMod.Ref();
                bMod = (ScRefUpdate::UpdateTranspose( pDocument, rSource,
                    rDest, rRef ) != UR_NOTHING || bPosChanged);
            }
            if ( bMod )
            {
                t->CalcRelFromAbs( aPos );
                bRefChanged = sal_True;
            }
        }
    }

    if (pShared)            // Shared Formula gegen echte Formel austauschen
    {
        pDocument->RemoveFromFormulaTree( this );   // update formula count
        delete pCode;
        pCode = new ScTokenArray( *pShared->GetCode() );
        bRefChanged = sal_True;
        pCode->Reset();
        while( (t = static_cast<ScToken*>(pCode->GetNextReference())) != NULL )
        {
            if( t->GetType() != svIndex )
            {
                t->CalcAbsIfRel( aOldPos );
                sal_Bool bMod;
                {   // own scope for SingleDoubleRefModifier dtor if SingleRef
                    SingleDoubleRefModifier aMod( *t );
                    ScComplexRefData& rRef = aMod.Ref();
                    bMod = (ScRefUpdate::UpdateTranspose( pDocument, rSource,
                        rDest, rRef ) != UR_NOTHING || bPosChanged);
                }
                if ( bMod )
                    t->CalcRelFromAbs( aPos );
            }
        }
    }

    if (bRefChanged)
    {
        if (pUndoDoc)
        {
            ScFormulaCell* pFCell = new ScFormulaCell( pUndoDoc, aPos, pOld,
                    eTempGrammar, cMatrixFlag);
            pFCell->aResult.SetToken( NULL);  // to recognize it as changed later (Cut/Paste!)
            pUndoDoc->PutCell( aPos.Col(), aPos.Row(), aPos.Tab(), pFCell );
        }

        bCompile = sal_True;
        CompileTokenArray();                // ruft auch StartListeningTo
        SetDirty();
    }
    else
        StartListeningTo( pDocument );      // Listener wie vorher

    delete pOld;
}

void ScFormulaCell::UpdateGrow( const ScRange& rArea, SCCOL nGrowX, SCROW nGrowY )
{
    EndListeningTo( pDocument );

    sal_Bool bRefChanged = sal_False;
    ScToken* t;
    ScRangeData* pShared = NULL;

    pCode->Reset();
    while( (t = static_cast<ScToken*>(pCode->GetNextReferenceOrName())) != NULL )
    {
        if( t->GetOpCode() == ocName )
        {
            ScRangeData* pName = pDocument->GetRangeName()->FindIndex( t->GetIndex() );
            if (pName)
            {
                if (pName->IsModified())
                    bRefChanged = sal_True;
                if (pName->HasType(RT_SHAREDMOD))
                    pShared = pName;
            }
        }
        else if( t->GetType() != svIndex )
        {
            t->CalcAbsIfRel( aPos );
            sal_Bool bMod;
            {   // own scope for SingleDoubleRefModifier dtor if SingleRef
                SingleDoubleRefModifier aMod( *t );
                ScComplexRefData& rRef = aMod.Ref();
                bMod = (ScRefUpdate::UpdateGrow( rArea,nGrowX,nGrowY,
                    rRef ) != UR_NOTHING);
            }
            if ( bMod )
            {
                t->CalcRelFromAbs( aPos );
                bRefChanged = sal_True;
            }
        }
    }

    if (pShared)            // Shared Formula gegen echte Formel austauschen
    {
        pDocument->RemoveFromFormulaTree( this );   // update formula count
        delete pCode;
        pCode = new ScTokenArray( *pShared->GetCode() );
        bRefChanged = sal_True;
        pCode->Reset();
        while( (t = static_cast<ScToken*>(pCode->GetNextReference())) != NULL )
        {
            if( t->GetType() != svIndex )
            {
                t->CalcAbsIfRel( aPos );
                sal_Bool bMod;
                {   // own scope for SingleDoubleRefModifier dtor if SingleRef
                    SingleDoubleRefModifier aMod( *t );
                    ScComplexRefData& rRef = aMod.Ref();
                    bMod = (ScRefUpdate::UpdateGrow( rArea,nGrowX,nGrowY,
                        rRef ) != UR_NOTHING);
                }
                if ( bMod )
                    t->CalcRelFromAbs( aPos );
            }
        }
    }

    if (bRefChanged)
    {
        bCompile = sal_True;
        CompileTokenArray();                // ruft auch StartListeningTo
        SetDirty();
    }
    else
        StartListeningTo( pDocument );      // Listener wie vorher
}

sal_Bool lcl_IsRangeNameInUse(sal_uInt16 nIndex, ScTokenArray* pCode, ScRangeName* pNames)
{
    for (FormulaToken* p = pCode->First(); p; p = pCode->Next())
    {
        if (p->GetOpCode() == ocName)
        {
            if (p->GetIndex() == nIndex)
                return sal_True;
            else
            {
                //  RangeData kann Null sein in bestimmten Excel-Dateien (#31168#)
                ScRangeData* pSubName = pNames->FindIndex(p->GetIndex());
                if (pSubName && lcl_IsRangeNameInUse(nIndex,
                                    pSubName->GetCode(), pNames))
                    return sal_True;
            }
        }
    }
    return sal_False;
}

sal_Bool ScFormulaCell::IsRangeNameInUse(sal_uInt16 nIndex) const
{
    return lcl_IsRangeNameInUse( nIndex, pCode, pDocument->GetRangeName() );
}

void lcl_FindRangeNamesInUse(std::set<sal_uInt16>& rIndexes, ScTokenArray* pCode, ScRangeName* pNames)
{
    for (FormulaToken* p = pCode->First(); p; p = pCode->Next())
    {
        if (p->GetOpCode() == ocName)
        {
            sal_uInt16 nTokenIndex = p->GetIndex();
            rIndexes.insert( nTokenIndex );

            ScRangeData* pSubName = pNames->FindIndex(p->GetIndex());
            if (pSubName)
                lcl_FindRangeNamesInUse(rIndexes, pSubName->GetCode(), pNames);
        }
    }
}

void ScFormulaCell::FindRangeNamesInUse(std::set<sal_uInt16>& rIndexes) const
{
    lcl_FindRangeNamesInUse( rIndexes, pCode, pDocument->GetRangeName() );
}

void ScFormulaCell::ReplaceRangeNamesInUse( const ScRangeData::IndexMap& rMap )
{
    for( FormulaToken* p = pCode->First(); p; p = pCode->Next() )
    {
        if( p->GetOpCode() == ocName )
        {
            sal_uInt16 nIndex = p->GetIndex();
            ScRangeData::IndexMap::const_iterator itr = rMap.find(nIndex);
            sal_uInt16 nNewIndex = itr == rMap.end() ? nIndex : itr->second;
            if ( nIndex != nNewIndex )
            {
                p->SetIndex( nNewIndex );
                bCompile = sal_True;
            }
        }
    }
    if( bCompile )
        CompileTokenArray();
}

void ScFormulaCell::CompileDBFormula()
{
    for( FormulaToken* p = pCode->First(); p; p = pCode->Next() )
    {
        if ( p->GetOpCode() == ocDBArea
            || (p->GetOpCode() == ocName && p->GetIndex() >= SC_START_INDEX_DB_COLL) )
        {
            bCompile = sal_True;
            CompileTokenArray();
            SetDirty();
            break;
        }
    }
}

void ScFormulaCell::CompileDBFormula( sal_Bool bCreateFormulaString )
{
    // zwei Phasen, muessen (!) nacheinander aufgerufen werden:
    // 1. FormelString mit alten Namen erzeugen
    // 2. FormelString mit neuen Namen kompilieren
    if ( bCreateFormulaString )
    {
        sal_Bool bRecompile = sal_False;
        pCode->Reset();
        for ( FormulaToken* p = pCode->First(); p && !bRecompile; p = pCode->Next() )
        {
            switch ( p->GetOpCode() )
            {
                case ocBad:             // DB-Bereich evtl. zugefuegt
                case ocColRowName:      // #36762# falls Namensgleichheit
                case ocDBArea:          // DB-Bereich
                    bRecompile = sal_True;
                break;
                case ocName:
                    if ( p->GetIndex() >= SC_START_INDEX_DB_COLL )
                        bRecompile = sal_True;  // DB-Bereich
                break;
                default:
                    ; // nothing
            }
        }
        if ( bRecompile )
        {
            String aFormula;
            GetFormula( aFormula, formula::FormulaGrammar::GRAM_NATIVE);
            if ( GetMatrixFlag() != MM_NONE && aFormula.Len() )
            {
                if ( aFormula.GetChar( aFormula.Len()-1 ) == '}' )
                    aFormula.Erase( aFormula.Len()-1 , 1 );
                if ( aFormula.GetChar(0) == '{' )
                    aFormula.Erase( 0, 1 );
            }
            EndListeningTo( pDocument );
            pDocument->RemoveFromFormulaTree( this );
            pCode->Clear();
            SetHybridFormula( aFormula, formula::FormulaGrammar::GRAM_NATIVE);
        }
    }
    else if ( !pCode->GetLen() && aResult.GetHybridFormula().Len() )
    {
        Compile( aResult.GetHybridFormula(), sal_False, eTempGrammar );
        aResult.SetToken( NULL);
        SetDirty();
    }
}

void ScFormulaCell::CompileNameFormula( sal_Bool bCreateFormulaString )
{
    // zwei Phasen, muessen (!) nacheinander aufgerufen werden:
    // 1. FormelString mit alten RangeNames erzeugen
    // 2. FormelString mit neuen RangeNames kompilieren
    if ( bCreateFormulaString )
    {
        sal_Bool bRecompile = sal_False;
        pCode->Reset();
        for ( FormulaToken* p = pCode->First(); p && !bRecompile; p = pCode->Next() )
        {
            switch ( p->GetOpCode() )
            {
                case ocBad:             // RangeName evtl. zugefuegt
                case ocColRowName:      // #36762# falls Namensgleichheit
                    bRecompile = sal_True;
                break;
                default:
                    if ( p->GetType() == svIndex )
                        bRecompile = sal_True;  // RangeName
            }
        }
        if ( bRecompile )
        {
            String aFormula;
            GetFormula( aFormula, formula::FormulaGrammar::GRAM_NATIVE);
            if ( GetMatrixFlag() != MM_NONE && aFormula.Len() )
            {
                if ( aFormula.GetChar( aFormula.Len()-1 ) == '}' )
                    aFormula.Erase( aFormula.Len()-1 , 1 );
                if ( aFormula.GetChar(0) == '{' )
                    aFormula.Erase( 0, 1 );
            }
            EndListeningTo( pDocument );
            pDocument->RemoveFromFormulaTree( this );
            pCode->Clear();
            SetHybridFormula( aFormula, formula::FormulaGrammar::GRAM_NATIVE);
        }
    }
    else if ( !pCode->GetLen() && aResult.GetHybridFormula().Len() )
    {
        Compile( aResult.GetHybridFormula(), sal_False, eTempGrammar );
        aResult.SetToken( NULL);
        SetDirty();
    }
}

void ScFormulaCell::CompileColRowNameFormula()
{
    pCode->Reset();
    for ( FormulaToken* p = pCode->First(); p; p = pCode->Next() )
    {
        if ( p->GetOpCode() == ocColRowName )
        {
            bCompile = sal_True;
            CompileTokenArray();
            SetDirty();
            break;
        }
    }
}

// ============================================================================

