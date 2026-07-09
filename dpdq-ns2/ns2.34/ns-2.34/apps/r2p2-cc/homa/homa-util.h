//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#ifndef ns_homa_util_h
#define ns_homa_util_h

// copied from OMNET++ inet/src/inet/common/Compat.h
/**
 * A const version of check_and_cast<> that accepts pointers other than cObject*, too.
 * For compatibility; OMNeT++ 5.0 and later already contain this.
 */
template <class T, class P>
T check_and_cast(const P *p)
{
    if (!p)
        throw std::runtime_error("check_and_cast(): cannot cast NULL pointer to type " + typeid(T).name());
    T ret = dynamic_cast<T>(p);
    if (!ret)
    {
        const cObject *o = dynamic_cast<const cObject *>(p);
        if (o)
            throw std::runtime_error("check_and_cast(): cannot cast (" + o->getClassName() + ") " + o->getFullPath().c_str() + " to type " + typeid(T).name());
        else
            throw std::runtime_error("check_and_cast(): cannot cast " + typeid(P).name() + " to type " + typeid(T).name());
    }
    return ret;
}

// copied from OMNET++ inet/src/inet/common/Compat.h
/**
 * A check_and_cast<> that accepts pointers other than cObject*, too.
 * For compatibility; OMNeT++ 5.0 and later already contain this.
 */
template <class T, class P>
T check_and_cast(P *p)
{
    if (!p)
        throw std::runtime_error("check_and_cast(): cannot cast NULL pointer to type " + typeid(T).name());
    T ret = dynamic_cast<T>(p);
    if (!ret)
    {
        const cObject *o = dynamic_cast<const cObject *>(p);
        if (o)
            throw std::runtime_error("check_and_cast(): cannot cast (" + o->getClassName() + ") " + o->getFullPath().c_str() + " to type " + typeid(T).name());
        else
            throw std::runtime_error("check_and_cast(): cannot cast " + typeid(P).name() + " to type " + typeid(T).name());
    }
    return ret;
}

#endif
