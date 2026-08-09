/* as_coroutine_transform.mjs - write AssemblyScript coroutines as straight-line code.
 *
 * AssemblyScript has no async/await: `asc` does not parse the keywords at all,
 * so there is no compiler transform of its own to lean on. Without this, a
 * coroutine in AS has to be hand-written as a `pc` switch over a struct of
 * hoisted locals -- correct, but the shape of the machine buries the shape of
 * the logic.
 *
 * This is that lowering, done by the compiler instead of by hand:
 *
 *     @coroutine
 *     function fetchTwice(a: Future, b: Future): i32 {
 *         let x: i32 = awaitValue(a);
 *         let y: i32 = awaitValue(b);
 *         return x + y;
 *     }
 *
 * becomes a Task subclass whose resume() is a state machine, with every local
 * lifted into a field so it survives suspension.
 *
 * Two annotations, no magic names:
 *   @coroutine  on a function, marking it for this lowering.
 *   @suspend    on a function, marking a CALL to it as a suspension point.
 *
 * (@suspend rather than @yield only because `yield` is a reserved word in asc's
 * tokenizer and will not parse as a decorator name.)
 *
 * A @suspend function's last parameter is an out Box and it returns a status:
 * AWAIT_PENDING (park and resume here), 0 (settled, value in the box), or a
 * negative status (the coroutine fails with it). That is the contract
 * Future.await already implements; @yield just names it for the compiler.
 *
 * WHAT IT REFUSES. Everything it cannot prove it lowers correctly is a hard
 * error naming the file, line and reason -- never a silent miscompile. A driver
 * that miscompiles because a transform guessed is far worse than one that will
 * not build. Phase 1 supports if/else, while, for, break, continue, return,
 * assignments and calls; it rejects do/while, switch, try, closures, untyped
 * locals, and suspension points anywhere but the top level of a statement.
 *
 * The lowering runs at afterParse, where the body AST is available and types
 * are not. It needs no types: locals carry their annotations, which is why
 * untyped ones are refused rather than inferred.
 */

import { Transform } from "assemblyscript/transform";

/*
 * Node kinds are calibrated from the compiler this transform is running inside,
 * rather than imported. Two things rule out the obvious alternatives: asc ships
 * minified, so constructor names are meaningless ("gn", "el"), and NodeKind's
 * numbering is not stable across versions -- this repo pins 0.28.17 while a
 * globally installed asc may be older, and importing the wrong copy silently
 * mismatches every comparison rather than failing loudly. Parsing a probe file
 * and reading the kinds off known positions is immune to both.
 */
let NodeKind = null;
let ASSIGN_TOKEN = null;

const PROBE_PATH = "__wasmos_coroutine_probe";
const PROBE_SOURCE = `function p(a: i32): i32 {
  let v: i32 = 0;
  if (a) { v = 1; } else { v = 2; }
  while (a) { break; }
  for (let i: i32 = 0; i < 1; ++i) { continue; }
  p(a);
  return v;
}`;

function calibrate(parser) {
    if (NodeKind) return;
    const before = parser.sources.length;
    parser.parseFile(PROBE_SOURCE, PROBE_PATH, false);
    const source = parser.sources.find((s) => s.normalizedPath.includes(PROBE_PATH));
    if (!source) throw new Error("@coroutine: could not calibrate the AST kinds");

    const fn = source.statements[0].declaration ?? source.statements[0];
    const body = fn.body.statements;
    const ifStatement = body[1];
    const whileStatement = body[2];
    const forStatement = body[3];
    const callStatement = body[4];
    const assignment = ifStatement.ifTrue.statements[0];

    NodeKind = {
        FunctionDeclaration: fn.kind,
        Variable: body[0].kind,
        If: ifStatement.kind,
        Block: ifStatement.ifTrue.kind,
        While: whileStatement.kind,
        Break: whileStatement.body.statements[0].kind,
        For: forStatement.kind,
        Continue: forStatement.body.statements[0].kind,
        Expression: callStatement.kind,
        Call: callStatement.expression.kind,
        Identifier: callStatement.expression.expression.kind,
        Return: body[5].kind,
        Literal: body[0].declarations[0].initializer.kind,
        Binary: assignment.expression.kind,
    };
    ASSIGN_TOKEN = assignment.expression.operator;

    /* Drop the probe again so it is never compiled. */
    parser.sources.splice(parser.sources.indexOf(source), 1);
    while (parser.sources.length > before) parser.sources.pop();
}

/* Emitted names are prefixed so they cannot collide with a user's locals. */
const PC = "__co_pc";
const BOX = "__co_box";
const STATUS = "__co_status";

/* The lowering injects its own aliased imports rather than relying on what the
 * file happens to import, so a @coroutine needs no particular import list and a
 * missing one cannot surface as a confusing error inside generated code. The
 * aliases cannot collide with a user's names. */
/* Only injected when an exported @coroutine needs the main pump, so a module
 * that merely uses coroutines internally never pulls in the event loop. */
const MAIN_IMPORT = "import { coroutineMain as __co_main } from \"./eventloop\";\n";

const RUNTIME_IMPORT =
    "import { Task as __co_Task, Box as __co_BoxType, AWAIT_PENDING as __co_AWAIT_PENDING, " +
    "TASK_COMPLETE as __co_TASK_COMPLETE, TASK_YIELDED as __co_TASK_YIELDED } from \"./coroutine\";\n";

/* A suspension's value comes back through a Box, which carries a usize. Getting
 * it into the declared type is a numeric cast for a primitive and a
 * reinterpretation for a reference, and asc accepts only the right one for each.
 * The type is known here only as source text, so the choice is made by name --
 * a closed list, since AS's primitives are a closed set. */
const PRIMITIVE_TYPES = new Set([
    "i8", "i16", "i32", "i64", "isize",
    "u8", "u16", "u32", "u64", "usize",
    "f32", "f64", "bool",
]);

function fromBox(type, expression) {
    return PRIMITIVE_TYPES.has(type.trim())
        ? `<${type}>${expression}`
        : `changetype<${type}>(${expression})`;
}

class CoroutineError extends Error {}

function fail(source, node, message) {
    const line = source.text.slice(0, node.range.start).split("\n").length;
    throw new CoroutineError(`${source.normalizedPath}:${line}: @coroutine: ${message}`);
}

function textOf(source, node) {
    return source.text.slice(node.range.start, node.range.end);
}

function decoratorNames(node) {
    return (node.decorators ?? []).map((d) => d.name?.text ?? "");
}

/* Walks every node reachable from `node`, so a scan does not have to know the
 * shape of each kind. AST nodes hold children in plain properties and arrays. */
function walk(node, visit) {
    if (!node || typeof node !== "object") return;
    if (Array.isArray(node)) {
        for (const child of node) walk(child, visit);
        return;
    }
    if (typeof node.kind !== "number") return;
    visit(node);
    for (const key of Object.keys(node)) {
        if (key === "range" || key === "parent" || key === "tokenizer") continue;
        walk(node[key], visit);
    }
}

/**
 * Rewrites an expression's source text so references to hoisted names become
 * field accesses. Done by splicing at the identifiers' own source offsets
 * rather than by string replacement, which would also hit substrings, property
 * names and string literals.
 */
function renderExpression(source, node, hoisted) {
    /* An identifier that names a PROPERTY is not a reference to anything in
     * scope: `startup.procEndpoint()` must not become
     * `startup.this.procEndpoint()` just because a local happens to be called
     * procEndpoint. Collected structurally rather than by node kind, so it needs
     * no calibration and covers every node with a `property`. */
    const properties = new Set();
    walk(node, (child) => {
        const property = child.property;
        if (property && typeof property === "object" && property.range) {
            properties.add(property.range.start);
        }
    });
    const sites = [];
    walk(node, (child) => {
        if (child.kind !== NodeKind.Identifier) return;
        if (!hoisted.has(child.text)) return;
        if (properties.has(child.range.start)) return;
        sites.push(child.range.start);
    });
    const start = node.range.start;
    let out = "";
    let cursor = start;
    for (const site of sites.sort((a, b) => a - b)) {
        if (site < cursor) continue;
        out += source.text.slice(cursor, site) + "this.";
        cursor = site;
    }
    return out + source.text.slice(cursor, node.range.end);
}

/** The call is a suspension point when its callee is a @yield function. */
function yieldCallIn(node, yieldFunctions) {
    let found = null;
    walk(node, (child) => {
        if (child.kind !== NodeKind.Call) return;
        const callee = child.expression;
        const name = callee?.kind === NodeKind.Identifier
            ? callee.text
            : callee?.property?.text;
        if (name && yieldFunctions.has(name)) found = child;
    });
    return found;
}

class Lowering {
    constructor(source, declaration, yieldFunctions, exported) {
        this.source = source;
        this.declaration = declaration;
        /* Read off the source text rather than a flags bitfield, for the same
         * reason the node kinds are calibrated: the numbering is not stable
         * across asc versions. */
        this.exported = exported;
        this.yieldFunctions = yieldFunctions;
        this.blocks = [[]];
        this.current = 0;
        this.hoisted = new Map(); /* name -> declared type */
        this.loops = [];
        /* Parameters live on the frame too, so a reference to one after a
         * suspension resolves to the field rather than a dead argument. */
        for (const parameter of declaration.signature.parameters) {
            this.hoisted.set(parameter.name.text, textOf(source, parameter.type));
        }
    }

    newBlock() {
        this.blocks.push([]);
        return this.blocks.length - 1;
    }

    emit(line) {
        this.blocks[this.current].push(line);
    }

    jump(target) {
        this.emit(`this.${PC} = ${target}; continue;`);
    }

    expr(node) {
        return renderExpression(this.source, node, this.hoisted);
    }

    /* A suspension point may only be the whole right-hand side of a statement.
     * Nested in a larger expression it would have to be re-evaluated on resume
     * along with everything around it, so it is refused and the caller hoists. */
    checkNoNestedYield(node, what) {
        if (node && yieldCallIn(node, this.yieldFunctions)) {
            fail(this.source, node, `a suspension point may not appear in ${what}; assign it to a local first`);
        }
    }

    /**
     * Emits a suspension. `pc` is set to this same block, so resuming re-runs
     * the call -- by then the future is settled and it returns the value. That
     * is why the arguments must be side-effect free, checked below.
     */
    lowerYield(call, assignTo, declaredType) {
        for (const arg of call.args) {
            /* Identifier, literal, or a dotted path -- anything whose
             * re-evaluation on resume cannot have an effect. */
            const simple = arg.kind === NodeKind.Identifier || arg.kind === NodeKind.Literal ||
                /^[A-Za-z_$][A-Za-z0-9_$.]*$/.test(textOf(this.source, arg));
            if (!simple) {
                fail(this.source, arg,
                     "a suspension point's arguments are re-evaluated when it resumes, so they must be " +
                     "plain identifiers, literals or property accesses");
            }
        }
        const callee = call.expression;
        const name = callee.kind === NodeKind.Identifier
            ? callee.text
            : `${this.expr(callee.expression)}.${callee.property.text}`;
        const args = call.args.map((a) => this.expr(a));
        args.push(`this.${BOX}`);

        const here = this.newBlock();
        this.jump(here);
        this.current = here;
        this.emit(`this.${STATUS} = ${name}(${args.join(", ")});`);
        this.emit(`if (this.${STATUS} == __co_AWAIT_PENDING) { this.${PC} = ${here}; return __co_TASK_YIELDED; }`);
        this.emit(`if (this.${STATUS} != 0) { return this.${STATUS}; }`);
        if (assignTo !== null) {
            this.emit(`${assignTo} = ${fromBox(declaredType, `this.${BOX}.value`)};`);
        }
    }

    lowerStatements(statements) {
        for (const statement of statements) this.lowerStatement(statement);
    }

    lowerStatement(statement) {
        switch (statement.kind) {
        case NodeKind.Block:
            this.lowerStatements(statement.statements);
            return;

        case NodeKind.Variable:
            for (const declaration of statement.declarations) {
                const name = declaration.name.text;
                if (!declaration.type) {
                    fail(this.source, declaration,
                         `local '${name}' needs an explicit type: it becomes a field on the ` +
                         "coroutine's frame, and this lowering runs before types are known");
                }
                const type = textOf(this.source, declaration.type);
                this.hoisted.set(name, type);
                const initializer = declaration.initializer;
                if (!initializer) return;
                const call = yieldCallIn(initializer, this.yieldFunctions);
                if (call) {
                    if (call !== initializer) {
                        fail(this.source, initializer,
                             "a suspension point must be the whole initializer, not part of an expression");
                    }
                    this.lowerYield(call, `this.${name}`, type);
                } else {
                    this.emit(`this.${name} = ${this.expr(initializer)};`);
                }
            }
            return;

        case NodeKind.Expression: {
            const inner = statement.expression;
            const call = yieldCallIn(inner, this.yieldFunctions);
            if (call) {
                if (call === inner) {
                    this.lowerYield(call, null, null);
                    return;
                }
                /* `x = await(...)` — an assignment whose entire RHS suspends. */
                if (inner.kind === NodeKind.Binary && inner.operator === ASSIGN_TOKEN &&
                    inner.right === call) {
                    const target = this.expr(inner.left);
                    const type = this.hoisted.get(inner.left.text);
                    if (!type) {
                        fail(this.source, inner,
                             "a suspension point may only be assigned to a local declared in this coroutine");
                    }
                    this.lowerYield(call, target, type);
                    return;
                }
                fail(this.source, inner,
                     "a suspension point must be a whole statement or the whole right-hand side of an assignment");
            }
            this.emit(`${this.expr(inner)};`);
            return;
        }

        case NodeKind.If: {
            this.checkNoNestedYield(statement.condition, "an if condition");
            const thenBlock = this.newBlock();
            const elseBlock = this.newBlock();
            const endBlock = this.newBlock();
            this.emit(`if (${this.expr(statement.condition)}) { this.${PC} = ${thenBlock}; continue; }`);
            this.jump(elseBlock);

            this.current = thenBlock;
            this.lowerStatement(statement.ifTrue);
            this.jump(endBlock);

            this.current = elseBlock;
            if (statement.ifFalse) this.lowerStatement(statement.ifFalse);
            this.jump(endBlock);

            this.current = endBlock;
            return;
        }

        case NodeKind.While: {
            this.checkNoNestedYield(statement.condition, "a while condition");
            const top = this.newBlock();
            const body = this.newBlock();
            const end = this.newBlock();
            this.jump(top);

            this.current = top;
            this.emit(`if (!(${this.expr(statement.condition)})) { this.${PC} = ${end}; continue; }`);
            this.jump(body);

            this.current = body;
            this.loops.push({ continueTarget: top, breakTarget: end });
            this.lowerStatement(statement.body);
            this.loops.pop();
            this.jump(top);

            this.current = end;
            return;
        }

        case NodeKind.For: {
            this.checkNoNestedYield(statement.condition, "a for condition");
            this.checkNoNestedYield(statement.incrementor, "a for incrementor");
            if (statement.initializer) this.lowerStatement(statement.initializer);
            const top = this.newBlock();
            const body = this.newBlock();
            const step = this.newBlock();
            const end = this.newBlock();
            this.jump(top);

            this.current = top;
            if (statement.condition) {
                this.emit(`if (!(${this.expr(statement.condition)})) { this.${PC} = ${end}; continue; }`);
            }
            this.jump(body);

            this.current = body;
            this.loops.push({ continueTarget: step, breakTarget: end });
            this.lowerStatement(statement.body);
            this.loops.pop();
            this.jump(step);

            this.current = step;
            if (statement.incrementor) this.emit(`${this.expr(statement.incrementor)};`);
            this.jump(top);

            this.current = end;
            return;
        }

        case NodeKind.Break:
            if (statement.label) fail(this.source, statement, "a labelled break is not supported");
            if (!this.loops.length) fail(this.source, statement, "break outside a loop");
            this.jump(this.loops[this.loops.length - 1].breakTarget);
            return;

        case NodeKind.Continue:
            if (statement.label) fail(this.source, statement, "a labelled continue is not supported");
            if (!this.loops.length) fail(this.source, statement, "continue outside a loop");
            this.jump(this.loops[this.loops.length - 1].continueTarget);
            return;

        case NodeKind.Return:
            this.checkNoNestedYield(statement.value, "a return value");
            if (statement.value) {
                this.emit(`out.value = <usize>(${this.expr(statement.value)});`);
            } else {
                this.emit("out.value = 0;");
            }
            this.emit("return __co_TASK_COMPLETE;");
            return;

        case NodeKind.Empty:
            return;

        default:
            fail(this.source, statement,
                 `this statement is not supported inside a @coroutine (node kind ${statement.kind}); ` +
                 "supported: if/else, while, for, break, continue, return, assignments and calls");
        }
    }

    /**
     * An EXPORTED @coroutine is the module's entry point, so the original
     * symbol has to keep its signature and actually run: it becomes a wrapper
     * that hands the task to the process pump. A non-exported one becomes a
     * factory instead, for a caller that drives it itself.
     */
    render(className) {
        this.lowerStatements(this.declaration.body.statements);
        /* Falling off the end completes with no value. */
        this.emit("out.value = 0;");
        this.emit("return __co_TASK_COMPLETE;");

        const parameters = this.declaration.signature.parameters.map((p) => ({
            name: p.name.text,
            type: textOf(this.source, p.type),
        }));

        const fields = [];
        for (const parameter of parameters) {
            fields.push(`    ${parameter.name}: ${parameter.type};`);
        }
        for (const [name, type] of this.hoisted) {
            if (parameters.some((p) => p.name === name)) continue;
            /* A reference field starts as a null of its own type rather than
             * `T | null`, so uses of it after the suspension need no non-null
             * assertion; nothing reads it before the suspension assigns it. */
            fields.push(PRIMITIVE_TYPES.has(type.trim())
                ? `    ${name}: ${type} = <${type}>0;`
                : `    ${name}: ${type} = changetype<${type}>(0);`);
        }

        const cases = this.blocks
            .map((lines, index) => `            case ${index}: {\n${lines.map((l) => `                ${l}`).join("\n")}\n            }`)
            .join("\n");

        /* The original name survives as a factory returning the Task, so callers
         * write `runtime.asyncStart(co, fetchTwice(a, b))` and never name the
         * generated class. */
        const name = this.declaration.name.text;
        const signature = parameters.map((p) => `${p.name}: ${p.type}`).join(", ");
        const arguments_ = parameters.map((p) => p.name).join(", ");
        const returnType = textOf(this.source, this.declaration.signature.returnType);

        const factory = this.exported
            ? `export function ${name}(${signature}): ${returnType} {
    return __co_main(new ${className}(${arguments_}));
}
`
            : `function ${name}(${signature}): ${className} {
    return new ${className}(${arguments_});
}
`;

        return `class ${className} extends __co_Task {
    ${PC}: i32 = 0;
    ${STATUS}: i32 = 0;
    ${BOX}: __co_BoxType = new __co_BoxType();
${fields.join("\n")}
    constructor(${parameters.map((p) => `${p.name}: ${p.type}`).join(", ")}) {
        super();
${parameters.map((p) => `        this.${p.name} = ${p.name};`).join("\n")}
    }

    resume(out: __co_BoxType): i32 {
        while (true) {
            switch (this.${PC}) {
${cases}
                default: { out.value = 0; return __co_TASK_COMPLETE; }
            }
        }
    }
}

${factory}`;
    }
}

export default class CoroutineTransform extends Transform {
    afterParse(parser) {
        calibrate(parser);
        const yieldFunctions = new Set();
        for (const source of parser.sources) {
            walk(source.statements, (node) => {
                if (!decoratorNames(node).includes("suspend")) return;
                const name = node.name?.text ?? node.declaration?.name?.text;
                if (name) yieldFunctions.add(name);
            });
        }

        for (const source of parser.sources) {
            const rewrites = [];
            for (const statement of source.statements) {
                const declaration = statement.declaration ?? statement;
                if (declaration.kind !== NodeKind.FunctionDeclaration) continue;
                if (!decoratorNames(declaration).includes("coroutine")) continue;
                if (!declaration.body) {
                    fail(source, declaration, "a @coroutine needs a body");
                }
                const className = `__Coroutine_${declaration.name.text}`;
                /* The decorator precedes the modifier in the statement's text,
                 * so the test is on the header up to the parameter list rather
                 * than on the first token. */
                const header = textOf(source, statement);
                const exported = /\bexport\s+function\b/.test(
                    header.slice(0, header.indexOf("(") + 1));
                const lowering = new Lowering(source, declaration, yieldFunctions, exported);
                rewrites.push({
                    start: statement.range.start,
                    end: statement.range.end,
                    text: lowering.render(className),
                    exported,
                });
            }
            if (!rewrites.length) continue;

            const needsMain = rewrites.some((r) => r.exported);
            let text = RUNTIME_IMPORT + (needsMain ? MAIN_IMPORT : "") + source.text;
            /* WASMOS_COROUTINE_DUMP=1 prints the generated machine, which is the
             * only practical way to see what a @coroutine actually became. */
            const dump = process.env.WASMOS_COROUTINE_DUMP;
            const shift = RUNTIME_IMPORT.length + (needsMain ? MAIN_IMPORT.length : 0);
            for (const rewrite of rewrites.sort((a, b) => b.start - a.start)) {
                text = text.slice(0, rewrite.start + shift) + rewrite.text +
                       text.slice(rewrite.end + shift);
            }
            if (dump) {
                console.error(`--- lowered ${source.normalizedPath} ---\n${text}\n--- end ---`);
            }
            /* Re-parsed as ordinary AssemblyScript, so the compiler type-checks
             * the lowering exactly as it would hand-written code rather than
             * this having to be trusted.
             *
             * parseFile ignores a path it has already seen, so the lowered text
             * is parsed under a unique path and then given the original's
             * identity -- otherwise the rewrite is silently dropped and the
             * untransformed source compiles instead. */
            const index = parser.sources.indexOf(source);
            const loweredPath = `${source.normalizedPath}.__lowered.ts`;
            parser.parseFile(text, loweredPath, source.sourceKind === 1);
            const lowered = parser.sources.find((s) => s.normalizedPath === loweredPath);
            if (!lowered) {
                throw new CoroutineError(`@coroutine: could not re-parse the lowered ${source.normalizedPath}`);
            }
            parser.sources.splice(parser.sources.indexOf(lowered), 1);
            lowered.normalizedPath = source.normalizedPath;
            lowered.internalPath = source.internalPath;
            lowered.simplePath = source.simplePath;
            lowered.sourceKind = source.sourceKind;
            parser.sources[index] = lowered;
        }
    }
}
