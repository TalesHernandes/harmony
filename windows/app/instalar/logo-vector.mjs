// Keep setup's DPI-independent geometry identical to the owner's source mark.
import {readFileSync,writeFileSync} from 'node:fs';
const source=new URL('../../../tools/harmony-icon.svg',import.meta.url);
const target=new URL('logo-vector.h',import.meta.url);
const rows=['// Generated from tools/harmony-icon.svg; coordinates remain vector geometry.',
 'void Logo(Gdiplus::Graphics& g,float x,float y,float side) {',
 '  auto saved=g.Save();g.TranslateTransform(x,y);g.ScaleTransform(side/903,side/903);g.TranslateTransform(-96,-101);'];
for(const match of readFileSync(source,'utf8').matchAll(/<path\b[^>]*\bd="([^"]+)"[^>]*\bfill="(#[\da-f]+)"[^>]*\/>/gi)) {
 const tokens=match[1].match(/[MCZ]|-?\d+(?:\.\d+)?/g);let i=0,position;
 rows.push('  { Gdiplus::GraphicsPath p;');
 while(i<tokens.length) {
  const command=tokens[i++];
  if(command==='M') {position=tokens.slice(i,i+2);i+=2;rows.push('    p.StartFigure();');}
  else if(command==='Z') rows.push('    p.CloseFigure();');
  else {
   if(command!=='C') i--;
   const values=tokens.slice(i,i+6);i+=6;
   rows.push('    p.AddBezier('+[...position,...values].map(v=>Number(v).toFixed(Number(v)%1?2:1).replace(/(\.\d*[1-9])0+$/,'$1')+'f').join(',')+');');
   position=values.slice(-2);
  }
 }
 rows.push('    Gdiplus::SolidBrush b(Gdiplus::Color(0xff'+match[2].slice(1)+'));g.FillPath(&b,&p); }');
}
rows.push('  g.Restore(saved);','}');
const generated=rows.join('\n')+'\n';
if(process.argv.includes('--check')) {
 if(readFileSync(target,'utf8').replace(/\r\n/g,'\n')!==generated) throw Error('Regenerate setup logo: node windows/app/instalar/logo-vector.mjs');
} else writeFileSync(target,generated);
