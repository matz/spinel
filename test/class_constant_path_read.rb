# Issue #52: reading a class-scoped constant from outside the class
# via `ClassName::CONST` used to emit `User_MAX_NAME_LENGTH` as an
# undeclared C identifier. Now it's emitted as `cst_User_MAX_NAME_LENGTH`
# with a matching declaration.

class User
  MAX_NAME_LENGTH = 20
end

puts User::MAX_NAME_LENGTH    # 20

# Mixed reads: class const, module const, top-level const all
# accessible via the same path forms.
TOP = 1

module Site
  ROOT = 2

  class Page
    LIMIT = 3
  end
end

puts TOP                      # 1
puts Site::ROOT               # 2
puts Site::Page::LIMIT        # 3
