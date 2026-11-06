# A load-record walk along a superclass chain reaches the chain's end
# however deep it is. The record bounded each such walk at 64 classes and
# answered "not an ancestor" past it, so a method change on the root of a
# deeper chain dropped out of the calls below it, which then ran the last
# definition. Every answer here is the one the chain's depth does not change.
class C0; def v = 1; end
class C1 < C0; end
class C2 < C1; end
class C3 < C2; end
class C4 < C3; end
class C5 < C4; end
class C6 < C5; end
class C7 < C6; end
class C8 < C7; end
class C9 < C8; end
class C10 < C9; end
class C11 < C10; end
class C12 < C11; end
class C13 < C12; end
class C14 < C13; end
class C15 < C14; end
class C16 < C15; end
class C17 < C16; end
class C18 < C17; end
class C19 < C18; end
class C20 < C19; end
class C21 < C20; end
class C22 < C21; end
class C23 < C22; end
class C24 < C23; end
class C25 < C24; end
class C26 < C25; end
class C27 < C26; end
class C28 < C27; end
class C29 < C28; end
class C30 < C29; end
class C31 < C30; end
class C32 < C31; end
class C33 < C32; end
class C34 < C33; end
class C35 < C34; end
class C36 < C35; end
class C37 < C36; end
class C38 < C37; end
class C39 < C38; end
class C40 < C39; end
class C41 < C40; end
class C42 < C41; end
class C43 < C42; end
class C44 < C43; end
class C45 < C44; end
class C46 < C45; end
class C47 < C46; end
class C48 < C47; end
class C49 < C48; end
class C50 < C49; end
class C51 < C50; end
class C52 < C51; end
class C53 < C52; end
class C54 < C53; end
class C55 < C54; end
class C56 < C55; end
class C57 < C56; end
class C58 < C57; end
class C59 < C58; end
class C60 < C59; end
class C61 < C60; end
class C62 < C61; end
class C63 < C62; end
class C64 < C63; end
class C65 < C64; end
class C66 < C65; end
class C67 < C66; end
class C68 < C67; end
class C69 < C68; end
class C70 < C69; end

# a class method of the root redefined, called through the deepest class
def C0.k = :a
def g = C70.k
p g
def C0.k = :b
p g

# an instance method of the root redefined, asked of an object of the
# deepest class at the top level and from its class body
o = C70.new
p o.v
class C70; p new.v; end
class C0; def v = 2; end
p o.v
class C70; p new.v; end

# an instance method the root defines after the object exists
class C0; def w = 1; end
p o.w
class C0; def w = 2; end
p o.w
