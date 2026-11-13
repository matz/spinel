# Data.define(*B::K.keys) where B = Bolt is written only after the read:
# CRuby raises NameError (B is not yet defined there). The path's parent
# names no class where the read runs, so the members are not read: refused.
class Bolt; K = { s: 1 }.freeze; end
F = Data.define(*B::K.keys)
p F.members
B = Bolt
