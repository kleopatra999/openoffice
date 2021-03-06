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



#ifndef __FRAMEWORK_UIELEMENT_ITEMCONTAINER_HXX_
#define __FRAMEWORK_UIELEMENT_ITEMCONTAINER_HXX_

//_________________________________________________________________________________________________________________
//	my own includes
//_________________________________________________________________________________________________________________

#include <threadhelp/threadhelpbase.hxx>
#include <macros/generic.hxx>
#include <macros/xinterface.hxx>
#include <macros/xtypeprovider.hxx>
#include <helper/shareablemutex.hxx>

//_________________________________________________________________________________________________________________
//	interface includes
//_________________________________________________________________________________________________________________
#include <com/sun/star/container/XIndexContainer.hpp>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/lang/XUnoTunnel.hpp>

//_________________________________________________________________________________________________________________
//	other includes
//_________________________________________________________________________________________________________________
#include <rtl/ustring.hxx>
#include <cppuhelper/implbase1.hxx>

#include <vector>
#include <fwidllapi.h>

namespace framework
{
class ConstItemContainer;
class FWI_DLLPUBLIC ItemContainer :   public ::cppu::WeakImplHelper1< ::com::sun::star::container::XIndexContainer>
{
    friend class ConstItemContainer;

    public:
        ItemContainer( const ShareableMutex& );
        ItemContainer( const ConstItemContainer& rConstItemContainer, const ShareableMutex& rMutex );
        ItemContainer( const com::sun::star::uno::Reference< com::sun::star::container::XIndexAccess >& rItemAccessContainer, const ShareableMutex& rMutex );
        virtual ~ItemContainer();

        //---------------------------------------------------------------------------------------------------------
        //	XInterface, XTypeProvider
        //---------------------------------------------------------------------------------------------------------
	    // XUnoTunnel
	    static const ::com::sun::star::uno::Sequence< sal_Int8 >&	GetUnoTunnelId() throw();
	    static ItemContainer*								        GetImplementation( const ::com::sun::star::uno::Reference< ::com::sun::star::uno::XInterface >& rxIFace ) throw();
	    sal_Int64													SAL_CALL getSomething( const ::com::sun::star::uno::Sequence< sal_Int8 >& rIdentifier ) throw(::com::sun::star::uno::RuntimeException);

        // XIndexContainer
		virtual void SAL_CALL insertByIndex( sal_Int32 Index, const ::com::sun::star::uno::Any& Element )
			throw (::com::sun::star::lang::IllegalArgumentException, ::com::sun::star::lang::IndexOutOfBoundsException, ::com::sun::star::lang::WrappedTargetException, ::com::sun::star::uno::RuntimeException);

		virtual void SAL_CALL removeByIndex( sal_Int32 Index )
			throw (::com::sun::star::lang::IndexOutOfBoundsException, ::com::sun::star::lang::WrappedTargetException, ::com::sun::star::uno::RuntimeException);

		// XIndexReplace
		virtual void SAL_CALL replaceByIndex( sal_Int32 Index, const ::com::sun::star::uno::Any& Element )
			throw (::com::sun::star::lang::IllegalArgumentException, ::com::sun::star::lang::IndexOutOfBoundsException, ::com::sun::star::lang::WrappedTargetException, ::com::sun::star::uno::RuntimeException);

		// XIndexAccess
		virtual sal_Int32 SAL_CALL getCount()
			throw (::com::sun::star::uno::RuntimeException);

		virtual ::com::sun::star::uno::Any SAL_CALL getByIndex( sal_Int32 Index )
			throw (::com::sun::star::lang::IndexOutOfBoundsException, ::com::sun::star::lang::WrappedTargetException, ::com::sun::star::uno::RuntimeException);

		// XElementAccess
		virtual ::com::sun::star::uno::Type SAL_CALL getElementType()
			throw (::com::sun::star::uno::RuntimeException)
		{
			return ::getCppuType((com::sun::star::uno::Sequence< com::sun::star::beans::PropertyValue >*)0);
		}

		virtual sal_Bool SAL_CALL hasElements()
			throw (::com::sun::star::uno::RuntimeException);

    private:
        ItemContainer();
        void copyItemContainer( const std::vector< com::sun::star::uno::Sequence< com::sun::star::beans::PropertyValue > >& rSourceVector, const ShareableMutex& rMutex );
        com::sun::star::uno::Reference< com::sun::star::container::XIndexAccess > deepCopyContainer( const com::sun::star::uno::Reference< com::sun::star::container::XIndexAccess >& rSubContainer, const ShareableMutex& rMutex );

        mutable ShareableMutex                                                               m_aShareMutex;
        std::vector< com::sun::star::uno::Sequence< com::sun::star::beans::PropertyValue > > m_aItemVector;
};

}

#endif // #ifndef __FRAMEWORK_UIELEMENT_ITEMCONTAINER_HXX_
