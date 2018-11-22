
#include "object.h"
#include "category.h"

bool Object::addToCategory(UserCategory *c)
{
    if (m_cats == nullptr)
        m_cats = new cat_list;
    if (!c->addObject(this))
        return false;
    m_cats->insert(c);
    return true;
}

bool Object::removeFromCategory(UserCategory *c)
{
    if (!inCategory(c))
        return false;
    m_cats->erase(c);
    c->removeObject(this);
    if (m_cats->empty())
    {
        delete m_cats;
        m_cats = nullptr;
    }
    return true;
}

bool Object::inCategory(UserCategory *c) const
{
    if (m_cats == nullptr)  return false;
    return m_cats->count(c);
}

bool Object::addToCategory(const std::string &s)
{
    UserCategory *c = UserCategory::find(s);
    if (c == nullptr)
        return false;
    return addToCategory(c);
}

bool Object::removeFromCategory(const std::string &s)
{
    UserCategory *c = UserCategory::find(s);
    if (c == nullptr)
        return false;
    return removeFromCategory(c);
}

bool Object::inCategory(const std::string &s) const
{
    UserCategory *c = UserCategory::find(s);
    if (c == nullptr)
        return false;
    return inCategory(c);
}
