# private_constant, public_constant and deprecate_constant raise NameError
# for a name that is not a constant of the class's own table. Spinel
# skipped the declaration, so no error was raised.
class P
  A = 1
  class Inner; end
  private_constant :A, :Inner
  begin
    private_constant :Nope
  rescue NameError => e
    p e.message
  end
  begin
    deprecate_constant "Gone"
  rescue NameError => e
    p e.message
  end
end

module M
  X = 1
  public_constant :X
  begin
    public_constant :X, :Y
  rescue NameError => e
    p e.message
  end
end

module Ns; class Deep; end; end
class Ns::Deep
  B = 2
  private_constant :B
  begin
    private_constant :C
  rescue NameError => e
    p e.message
  end
end
p :done
