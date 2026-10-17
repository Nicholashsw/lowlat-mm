/ Fill markouts for a lowlat-mm blotter: where the mid went after each fill.

/   q analytics/markouts.q -feed data/es_feed.csv -fills trades.csv [-out markouts.csv]

/ feed:  ts_ns,symbol,bid_px,bid_sz,ask_px,ask_sz   (the engine's input)
/ fills: ts_ns,order_id,side,px_ticks,qty          (run_backtest's trades.csv, or a
/        blotter from the OCaml replay: same layout, so fill rules compare directly)

/ The markout at horizon h is side * (mid at fill time + h - fill price), in ticks:
/ positive means the fill was worth something h later. The prevailing mid at a
/ time comes from an as-of join (aj). The 0 ms markout is the half spread
/ captured and its decay out to five minutes is the adverse selection. A window
/ join (wj) adds the worst and best mid over the minute after each fill.
/ Times are the blotter's, i.e. when the strategy learns of the fill.
/ analytics/markouts.py computes the same table in pandas.

args:.Q.opt .z.x;
opt:{[k;d] $[k in key args; first args k; 10h=type d; d; [-2"missing -",string k; exit 1]]};
feedf:hsym `$opt[`feed;`];
fillf:hsym `$opt[`fills;`];
outf:hsym `$opt[`out;"markouts.csv"];

epoch:946684800000000000;                     / ns from 1970.01.01 to kdb's 2000.01.01
tots:{"p"$x-epoch};

q:("JSJJJJ";enlist ",") 0: feedf;
q:select sym:symbol, time:tots ts_ns, mid:0.5*bid_px+ask_px from q;
q:update `g#sym from `sym`time xasc 0!select by sym, time from q;     / last quote per timestamp, as markouts.py

f:("JJCJJ";enlist ",") 0: fillf;
f:select sym:first q`sym, time:tots ts_ns, id:order_id, side:?[side="B";1;-1], px:px_ticks, qty from f;

H:`0ms`1ms`100ms`1s`10s`1m`5m!"n"$1000000*0 1 100 1000 10000 60000 300000;
mocols:`$"mo_",/:string key H;
midat:{[h] exec mid from aj[`sym`time;select sym, time:time+h from f;q]};
f:f,'flip mocols!{f[`side]*midat[x]-f`px} each value H;

w:(f`time;f[`time]+0D00:01:00.000000000);
ex:wj[w;`sym`time;select sym, time from f;(select sym, time, lo:mid, hi:mid from q;(min;`lo);(max;`hi))];
f:update mae_1m:?[side=1;ex[`lo]-px;px-ex`hi], mfe_1m:?[side=1;ex[`hi]-px;px-ex`lo] from f;

stats:{[t] select n_fills:count i, qty:sum qty,
    mo_0ms:avg mo_0ms, mo_1ms:avg mo_1ms, mo_100ms:avg mo_100ms, mo_1s:avg mo_1s,
    mo_10s:avg mo_10s, mo_1m:avg mo_1m, mo_5m:avg mo_5m,
    pos_5m_pct:100*avg mo_5m>0, mae_1m:avg mae_1m, mfe_1m:avg mfe_1m from t};
res:raze {[t;s] `side xcols update side:s from stats t}'[(f;select from f where side=1;select from f where side=-1);`all`buy`sell];

-1 "markouts in ticks, ",string[count f]," fills";
show res;
outf 0: csv 0: f;
-1 "per-fill markouts written to ",string outf;
if[not `keep in key args; exit 0];
