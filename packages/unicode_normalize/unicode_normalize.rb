# Spinel package: unicode_normalize
#
# String#unicode_normalize, #unicode_normalize! and #unicode_normalized?,
# over CRuby's own implementation (unicode_normalize/normalize.rb and the
# tables CRuby generates from the Unicode data, unicode_normalize/tables.rb,
# both as CRuby 4.0 ships them). CRuby loads them the first time one of the
# three methods runs; Spinel splices this package into a program whose source
# calls one, so a program that does not carries none of the tables.
#
# The changes to CRuby's files: a buffer the methods append to starts as +''
# (Spinel's literals are frozen), and the `frozen_string_literal: false` and
# `coding: us-ascii` comments, which Spinel would only warn about, are gone.
require_relative "unicode_normalize/normalize"

class String
  def unicode_normalize(form = :nfc)
    UnicodeNormalize.normalize(self, form)
  end

  def unicode_normalize!(form = :nfc)
    replace(UnicodeNormalize.normalize(self, form))
  end

  def unicode_normalized?(form = :nfc)
    UnicodeNormalize.normalized?(self, form)
  end
end
