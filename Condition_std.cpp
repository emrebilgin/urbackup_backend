/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2016 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "Condition_std.h"
#include "Mutex_std.h"
#include "Server.h"

void CCondition::wait(IScopedLock *lock, int timems)
{
	std::unique_lock<std::recursive_mutex> *tl=((CLock*)lock->getLock())->getLock();

	if (timems < 0)
	{
		cond.wait(*tl);
	}
	else
	{
		cond.wait_for(*tl, std::chrono::milliseconds(timems));
	}
}

void CCondition::notify_one()
{
	cond.notify_one();
}

void CCondition::notify_all()
{
	cond.notify_all();
}