/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * LexerGlue.h
 * Captures errors while parsing the schema.
 * Copyright (C) 2014 Simon Newton
 */

#ifndef COMMON_WEB_LEXERGLUE_H_
#define COMMON_WEB_LEXERGLUE_H_

#include <ola/base/Macro.h>
#include <string>
#include <stack>

#include "ola/web/Json.h"
#include "ola/web/JsonLexer.h"

namespace ola {
namespace web {

class LexerGlue {
 public:
  LexerGlue(JsonParserInterface *parser);

  void String(const char *text);

  void Bool(bool value);

  void Int(const char *text);

  void Fractional(const char *text);

  void Exponent(const char *text);

  void Null();

  void OpenArray();

  void CloseArray();

  void OpenObject();

  void CloseObject();

  void Comma();

  void Colon();

 private:
  // An 8-state state machine.
  enum Expect {
    START,
    ARRAY_ELEMENT,
    ARRAY_COMMA,
    OBJECT_KEY,
    OBJECT_COLON,
    OBJECT_ELEMENT,
    OBJECT_COMMA,
    END,
  };

  std::stack<Expect> m_expect_stack;
  JsonParserInterface *m_parser;
  JsonDoubleValue::DoubleRepresentation m_number;

  DISALLOW_COPY_AND_ASSIGN(LexerGlue);
};
}  // namespace web
}  // namespace ola
#endif  // COMMON_WEB_LEXERGLUE_H_
