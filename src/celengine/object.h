#pragma once

#include <string>
#include <set>

class UserCategory;

class Object
{
    typedef std::set<UserCategory*> cat_list;

    cat_list *m_cats;
protected:
    Object() : m_cats(nullptr) {}

public:
    bool addToCategory(UserCategory*);
    bool removeFromCategory(UserCategory*);
    bool inCategory(UserCategory*) const;

    bool addToCategory(const std::string&);
    bool removeFromCategory(const std::string&);
    bool inCategory(const std::string&) const;
    int inCategories() const { return m_cats == nullptr ? 0 : m_cats->size(); }
};
