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

package org.apache.openoffice.ooxml.schema.model.attribute;

import org.apache.openoffice.ooxml.schema.model.base.INode;
import org.apache.openoffice.ooxml.schema.model.base.INodeReference;
import org.apache.openoffice.ooxml.schema.model.base.INodeVisitor;
import org.apache.openoffice.ooxml.schema.model.base.Location;
import org.apache.openoffice.ooxml.schema.model.base.NodeType;
import org.apache.openoffice.ooxml.schema.model.base.QualifiedName;
import org.apache.openoffice.ooxml.schema.model.schema.Schema;

public class AttributeReference
    extends AttributeBase
    implements INodeReference
{
    public AttributeReference (
        final QualifiedName aReferencedName,
        final String sUse,
        final String sDefault,
        final String sFixed,
        final Location aLocation)
    {
        super(aReferencedName, sUse, sDefault, sFixed, aLocation);
        
        maReferencedName = aReferencedName;
    }




    public Attribute GetReferencedAttribute (final Schema aSchema)
    {
        final Attribute aAttribute = aSchema.Attributes.Get(maReferencedName);
        if (aAttribute == null)
            throw new RuntimeException(
                String.format(
                    "can not find attribute %s, referenced at %s",
                    maReferencedName.GetDisplayName(),
                    GetLocation()));
        return aAttribute;
    }
    
    
    
    
    @Override
    public INode GetReferencedNode (final Schema aSchema)
    {
        return GetReferencedAttribute(aSchema);
    }

        
    
    
    public QualifiedName GetReferencedName ()
    {
        return maReferencedName;
    }

    
    
    
    @Override
    public NodeType GetNodeType ()
    {
        return NodeType.AttributeReference;
    }




    @Override
    public void AcceptVisitor(INodeVisitor aVisitor)
    {
        aVisitor.Visit(this);
    }

    
    
    
    private final QualifiedName maReferencedName;
}
