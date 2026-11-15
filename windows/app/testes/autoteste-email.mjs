// Verify disposable accounts before media tests; delivery is asynchronous.
import {readdirSync,readFileSync} from 'node:fs';
import {resolve} from 'node:path';
export async function verifyFixtureEmail(response,email,base) {
  const cookie=response.headers.get('set-cookie')?.split(';')[0];
  const end=Date.now()+15000;
  while(Date.now()<end) {
    for(const file of readdirSync(process.env.HARMONY_E2E_MAIL_DIR).filter(f=>f.endsWith('.eml'))) {
      const mail=readFileSync(resolve(process.env.HARMONY_E2E_MAIL_DIR,file),'utf8');
      if(!mail.includes('To: <'+email+'>'))continue;
      const code=mail.match(/\b\d{6}\b/)?.[0];
      if(code && (await fetch(base+'/api/me/email/verify',{method:'POST',headers:{Cookie:cookie,'Content-Type':'application/json'},body:JSON.stringify({code})})).ok)return;
    }
    await new Promise(r=>setTimeout(r,50));
  }
  throw Error('Disposable account email verification timed out');
}
