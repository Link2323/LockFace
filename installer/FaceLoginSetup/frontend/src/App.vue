<script lang="ts" setup>
import { ref, onMounted, nextTick, computed } from 'vue'
import { GetDefaultPaths, Install, Uninstall, PickDirectory, IsInstalled, LaunchConsole, GetMode } from '../wailsjs/go/main/App'
import { EventsOn, EventsOff, Quit } from '../wailsjs/runtime'

const installDir = ref('')
const showInstall = ref(true)
const running = ref(false)
const progressPercent = ref(0)
const progressStep = ref('')
const progressStatus = ref('')
const progressDetail = ref('')
const resultMessage = ref('')
const resultSuccess = ref(false)
const showResult = ref(false)
const alreadyInstalled = ref(false)
const installDone = ref(false)
const launchChecked = ref(true)
const finishError = ref('')
const finishing = ref(false)
// Uninstaller build (or full installer run with --uninstall): land on the
// uninstall page; the slim build has no install capability at all.
const hideTabs = ref(false)
const licenseDialog = ref<HTMLDialogElement | null>(null)
const licenseDocuments = [
  { path: 'LICENSE.txt', label: '项目许可' },
  { path: 'THIRD_PARTY_NOTICES.txt', label: '第三方声明' },
  { path: 'MODEL_LICENSES.txt', label: '模型许可' },
  { path: 'PRIVACY.txt', label: '隐私说明' },
]
const licenseDocument = ref('LICENSE.txt')
const licenseText = ref('')
const licenseCache = new Map<string, string>()

const CONSENT_DOCUMENT = 'PRIVACY.txt'
const consentDialog = ref<HTMLDialogElement | null>(null)
const consentScroller = ref<HTMLElement | null>(null)
const consentText = ref('')
const consentLoading = ref(false)
const consentError = ref('')
const consentScrolled = ref(false)
const consentChecked = ref(false)
const consentAccepted = ref(false)

type ConsentBlock = {
  kind: 'title' | 'meta' | 'callout' | 'heading' | 'paragraph' | 'items'
  text: string
  items?: string[]
}

const ITEM_START = /^(?:\d+\.\d+|（[一二三四五六七八九十]+）|[一二三四五六七八九十]+、)/

// The canonical notice keeps each clause on one line (structure lines carry
// the parsing: article headings, x.y clause numbers, （一） items); clause
// text is not hard-wrapped. The continuation-join stays as a safety net for
// hand-edited or older wrapped copies, so a stray hard wrap cannot leave a
// visible gap at the right edge of every first line.
function splitNumberedItems(text: string): string[] | null {
  const lines = text.split('\n').map(line => line.trim()).filter(Boolean)
  if (lines.length <= 1) return null
  const hasItems = lines.some(line => ITEM_START.test(line))
  if (!hasItems) return null
  const items: string[] = []
  for (const line of lines) {
    if (ITEM_START.test(line) || items.length === 0) {
      items.push(line)
    } else {
      items[items.length - 1] += ' ' + line
    }
  }
  return items
}

function normalizeParagraph(text: string): string {
  return text.split('\n').map(line => line.trim()).filter(Boolean).join(' ')
}

function pushParagraph(blocks: ConsentBlock[], text: string) {
  const items = splitNumberedItems(text)
  if (items) blocks.push({ kind: 'items', text: '', items })
  else blocks.push({ kind: 'paragraph', text: normalizeParagraph(text) })
}

// The canonical notice is plain text (also shipped as PRIVACY.txt). Split it
// into readable blocks here instead of rendering a 9 KB <pre> wall: headings
// and the amber callout are presentation only, the text itself is untouched.
const consentBlocks = computed<ConsentBlock[]>(() => {
  const raw = consentText.value.replace(/\r\n/g, '\n').trim()
  if (!raw) return []
  const blocks: ConsentBlock[] = []
  let beforeNotice = true
  for (const paragraph of raw.split(/\n\s*\n/)) {
    const text = paragraph.trim()
    if (!text) continue
    const firstLine = text.split('\n')[0].trim()
    if (beforeNotice) {
      if (firstLine.startsWith('特别提示：')) {
        blocks.push({ kind: 'callout', text: text.replace(/\n+/g, ' ') })
        beforeNotice = false
        continue
      }
      if (firstLine.includes('隐私说明与知情同意书')) {
        blocks.push({ kind: 'title', text: firstLine })
        continue
      }
      let meta = ''
      for (const line of text.split('\n')) {
        const value = line.trim()
        if (!value) continue
        if (!meta) meta = value
        else if (/[、，；：]$/.test(meta)) meta += value
        else meta += ' · ' + value
      }
      if (meta) blocks.push({ kind: 'meta', text: meta })
      continue
    }
    if (/^第[一二三四五六七八九十百]+条\s/.test(firstLine)) {
      blocks.push({ kind: 'heading', text: firstLine })
      const rest = text.split('\n').slice(1).join('\n').trim()
      if (rest) pushParagraph(blocks, rest)
    } else {
      pushParagraph(blocks, text)
    }
  }
  return blocks
})

async function fetchDocument(path: string): Promise<string> {
  let text = licenseCache.get(path)
  if (text === undefined) {
    const response = await fetch(new URL(path, document.baseURI))
    if (!response.ok) throw new Error(`读取失败 (${response.status})`)
    text = await response.text()
    licenseCache.set(path, text)
  }
  return text
}

async function showLicenseDocument(path: string) {
  licenseDocument.value = path
  licenseText.value = '正在读取…'
  try {
    const text = await fetchDocument(path)
    if (licenseDocument.value === path) licenseText.value = text
  } catch (err: unknown) {
    if (licenseDocument.value === path) {
      licenseText.value = err instanceof Error ? err.message : '无法读取许可文件'
    }
  }
}

function refreshConsentGate() {
  if (consentAccepted.value) {
    consentScrolled.value = true
    return
  }
  // Never measure the loading placeholder: it is short enough to look like
  // "already at the end" and would latch the controls open before the notice
  // has rendered.
  if (consentLoading.value || !consentText.value || consentError.value) return
  // Latch once the end has been reached: scrolling back up to re-read a
  // clause must not hide the consent controls again.
  if (consentScrolled.value) return
  const el = consentScroller.value
  if (!el) return
  if (el.scrollHeight <= el.clientHeight + 4 ||
      el.scrollTop + el.clientHeight >= el.scrollHeight - 8) {
    consentScrolled.value = true
    // The footer grows when the controls appear, shrinking the scroll
    // viewport. Re-anchor to the real bottom so the last clause stays visible.
    void nextTick(() => {
      const scroller = consentScroller.value
      if (scroller) scroller.scrollTop = scroller.scrollHeight
    })
  }
}

function onConsentScroll() {
  refreshConsentGate()
}

async function loadConsentText() {
  if (consentText.value || consentLoading.value) return
  consentLoading.value = true
  consentError.value = ''
  try {
    consentText.value = await fetchDocument(CONSENT_DOCUMENT)
  } catch (err: unknown) {
    consentError.value = err instanceof Error ? err.message : '无法读取隐私说明'
  } finally {
    consentLoading.value = false
    await nextTick()
    refreshConsentGate()
  }
}

function openConsent() {
  const dialog = consentDialog.value
  if (!dialog) return
  if (consentAccepted.value) {
    consentChecked.value = true
    consentScrolled.value = true
  } else {
    consentChecked.value = false
    consentScrolled.value = false
    if (consentScroller.value) consentScroller.value.scrollTop = 0
  }
  if (!dialog.open) dialog.showModal()
  void loadConsentText()
  void nextTick(() => {
    // Do not let showModal() leave the initial focus on the decline button;
    // start on the document surface instead so no button gets a focus ring.
    consentScroller.value?.focus()
    refreshConsentGate()
  })
}

function acceptConsent() {
  if (!consentText.value || consentError.value ||
      !consentScrolled.value || !consentChecked.value) return
  consentAccepted.value = true
  consentChecked.value = true
  consentDialog.value?.close()
}

async function declineConsent() {
  consentDialog.value?.close()
  await doExit()
}

function openLicenses() {
  licenseDialog.value?.showModal()
  void showLicenseDocument(licenseDocument.value)
}

onMounted(async () => {
  let uninstallMode = false
  try {
    const mode = await GetMode()
    uninstallMode = !!mode.uninstall
    if (uninstallMode) {
      showInstall.value = false
    }
    if (mode.locked) {
      hideTabs.value = true
    }
  } catch {
    // GetMode failing is not fatal — stay on the install page.
  }
  try {
    const paths = await GetDefaultPaths()
    installDir.value = paths.installDir
    alreadyInstalled.value = await IsInstalled()
  } catch (err: any) {
    // Keep installDir non-empty so the install button isn't permanently disabled
    // on a marshalling failure — surface the error as the result banner instead.
    installDir.value = 'C:\\Program Files\\LockFace'
    resultMessage.value = '初始化失败：' + (err?.message || String(err))
    resultSuccess.value = false
    showResult.value = true
  }
  if (!uninstallMode) openConsent()
})

async function doPickDirectory() {
  try {
    const dir = await PickDirectory()
    if (dir) {
      installDir.value = dir
    }
  } catch (err: any) {
    resultMessage.value = '选择目录失败：' + (err?.message || String(err))
    resultSuccess.value = false
    showResult.value = true
  }
}

function toggleMode(mode: string) {
  showInstall.value = mode === 'install'
  showResult.value = false
  installDone.value = false
  finishError.value = ''
  progressStatus.value = ''
}

async function doInstall() {
  if (!consentAccepted.value) {
    openConsent()
    return
  }
  running.value = true
  showResult.value = false
  installDone.value = false
  finishError.value = ''
  progressPercent.value = 0
  progressStatus.value = 'running'

  // Clear any listener left over from a previous run that never reached 100%
  // (e.g. a backend failure path returns before emitting the final event),
  // then register a fresh one. Always torn down in the finally below.
  EventsOff('setup:progress')
  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = e.detail || ''
  })

  try {
    const result = await Install(installDir.value)
    resultMessage.value = result.message
    resultSuccess.value = result.success
    // Success goes to the dedicated finish page (checkbox + finish button);
    // failure keeps the old result banner with a back button.
    if (result.success) {
      installDone.value = true
    } else {
      showResult.value = true
    }

  } catch (err: any) {
    resultMessage.value = err?.message || String(err) || '安装失败'
    resultSuccess.value = false
    showResult.value = true
  } finally {
    running.value = false
    EventsOff('setup:progress')
  }
}

async function doFinish() {
  finishing.value = true
  finishError.value = ''
  try {
    if (launchChecked.value) {
      const r = await LaunchConsole()
      if (!r.success) {
        finishError.value = r.message || '打开注册向导失败'
        finishing.value = false
        return
      }
    }
    await Quit()
  } catch (err: any) {
    finishError.value = err?.message || String(err)
    finishing.value = false
  }
}

async function doExit() {
  try {
    await Quit()
  } catch {
    // Ignore — the window close button still works
  }
}

async function doUninstall() {
  if (!confirm('确定要卸载 LockFace 人脸登录吗？\n\n⚠️ 卸载将删除所有程序文件、人脸数据和日志，且不可恢复！')) return

  running.value = true
  showResult.value = false
  progressPercent.value = 0
  progressStatus.value = 'running'

  EventsOff('setup:progress')
  EventsOn('setup:progress', (e: any) => {
    progressPercent.value = e.percent
    progressStep.value = e.step
    progressStatus.value = e.status
    progressDetail.value = e.detail || ''
  })

  try {
    const result = await Uninstall()
    resultMessage.value = result.message
    resultSuccess.value = result.success
    showResult.value = true
  } catch (err: any) {
    resultMessage.value = err?.message || String(err) || '卸载失败'
    resultSuccess.value = false
    showResult.value = true
  } finally {
    running.value = false
    EventsOff('setup:progress')
  }
}
</script>

<template>
  <div class="flex flex-col h-screen bg-white select-none">
    <!-- Header -->
    <div class="px-8 pt-8 pb-2">
      <h1 class="text-2xl font-light tracking-tight text-gray-900">LockFace</h1>
      <p class="text-sm text-gray-400 font-light">人脸识别登录系统 · {{ showInstall ? '安装程序' : '卸载程序' }}</p>
    </div>

    <!-- Mode Tabs (hidden in the uninstaller build) -->
    <div v-if="!hideTabs" class="px-8 mt-4 flex gap-6 border-b border-gray-100">
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('install')"
        :disabled="running"
      >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      <button
        :class="['pb-2 text-sm font-medium transition-colors',
                 !showInstall ? 'text-gray-900 border-b-2 border-gray-900' : 'text-gray-400 hover:text-gray-600']"
        @click="toggleMode('uninstall')"
        :disabled="running"
      >卸载</button>
    </div>

    <!-- Body -->
    <div class="flex-1 px-8 py-6">
      <!-- Install mode -->
      <div v-if="showInstall && !running && !showResult && !installDone">
        <label class="block text-xs text-gray-500 uppercase tracking-wider mb-2">安装目录</label>
        <div class="flex items-center gap-3">
          <input
            v-model="installDir"
            class="flex-1 px-3 py-2 text-sm border border-gray-200 bg-gray-50
                   focus:outline-none focus:border-gray-400 transition-colors text-gray-800"
            placeholder="选择安装目录"
          />
          <button
            class="flex-shrink-0 w-9 h-9 flex items-center justify-center border border-gray-200
                   hover:bg-gray-100 transition-colors text-gray-500"
            @click="doPickDirectory"
            title="选择文件夹"
          >
            <svg xmlns="http://www.w3.org/2000/svg" class="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>
            </svg>
          </button>
        </div>
        <p class="mt-1 text-xs text-gray-400">建议保留默认目录：<code class="text-gray-500">C:\Program Files\</code> 受系统保护，可防止程序文件被篡改。</p>

        <div class="mt-5 border-t border-gray-100 pt-4">
          <div class="flex items-start justify-between gap-4">
            <div class="min-w-0">
              <p class="text-xs font-medium text-gray-800">人脸识别隐私说明</p>
              <p class="mt-1 text-xs leading-relaxed text-gray-500">
                人脸仅在本机用于登录比对，不保存照片或视频，不上传；只保存人脸特征和加密后的 Windows 密码。
              </p>
            </div>
            <button class="flex-shrink-0 text-xs text-gray-600 underline decoration-gray-300 underline-offset-4 hover:text-gray-900" @click="openConsent">
              {{ consentAccepted ? '重新阅读' : '阅读全文' }}
            </button>
          </div>
          <p class="mt-2 text-xs" :class="consentAccepted ? 'text-gray-700' : 'text-gray-500'">
            {{ consentAccepted ? '已阅读并同意《LockFace 人脸识别隐私说明与知情同意书》' : '安装前需完整阅读并同意隐私说明' }}
          </p>
        </div>

        <button
          class="mt-5 w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doInstall"
          :disabled="!installDir || !consentAccepted"
        >{{ alreadyInstalled ? '更新' : '安装' }}</button>
      </div>

      <!-- Uninstall mode -->
      <div v-if="!showInstall && !running && !showResult">
        <p class="text-sm text-gray-600 leading-relaxed">
          卸载将停止并删除 LockFace 服务、注销登录组件、删除全部程序文件，
          以及 <strong>人脸数据和日志（不可恢复）</strong>，并移除安装目录。
        </p>
        <button
          class="mt-6 w-full py-2.5 text-sm font-medium border border-gray-300 text-gray-700
                 hover:bg-gray-100 transition-colors disabled:opacity-40"
          @click="doUninstall"
        >卸载 LockFace</button>
      </div>

      <!-- Progress -->
      <div v-if="running" class="space-y-4">
        <div class="relative h-0.5 bg-gray-100">
          <div
            class="absolute top-0 left-0 h-full bg-gray-900 transition-all duration-300 ease-out"
            :style="{ width: progressPercent + '%' }"
          ></div>
        </div>
        <div class="flex items-center justify-between">
          <span class="text-sm text-gray-800">{{ progressStep }}</span>
          <span class="text-xs text-gray-400">{{ progressPercent }}%</span>
        </div>
        <p v-if="progressDetail" class="text-xs text-gray-400">{{ progressDetail }}</p>
      </div>

      <!-- Result -->
      <div v-if="showResult && !running" class="space-y-4">
        <div
          :class="['text-sm whitespace-pre-line leading-relaxed',
                   resultSuccess ? 'text-gray-800' : 'text-red-600']"
        >
          {{ resultSuccess ? '✓ ' : '✗ ' }}{{ resultMessage }}
        </div>
        <button
          class="w-full py-2 text-sm text-gray-500 border border-gray-200
                 hover:bg-gray-50 transition-colors"
          @click="hideTabs ? doExit() : (showResult = false)"
        >{{ hideTabs ? '退出' : '返回' }}</button>
      </div>

      <!-- Install finish page: success + optional console launch -->
      <div v-if="installDone && !running" class="space-y-6">
        <div class="flex items-center gap-4">
          <div class="w-10 h-10 rounded-full bg-green-50 flex items-center justify-center flex-shrink-0">
            <svg xmlns="http://www.w3.org/2000/svg" class="w-5 h-5 text-green-600" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
              <path d="M20 6 9 17l-5-5"/>
            </svg>
          </div>
          <div>
            <p class="text-lg font-medium text-gray-900">安装成功</p>
            <p class="text-sm text-gray-500 leading-relaxed">人脸认证服务已启动，登录界面已启用人脸选项。<br>注册人脸后即可使用人脸登录。</p>
          </div>
        </div>
        <label class="flex items-center gap-2.5 text-sm text-gray-700 cursor-pointer select-text">
          <input type="checkbox" v-model="launchChecked" class="w-4 h-4 accent-gray-900" />
          立即打开人脸注册向导
        </label>
        <p v-if="finishError" class="text-xs text-red-600">{{ finishError }}</p>
        <button
          class="w-full py-2.5 text-sm font-medium bg-gray-900 text-white
                 hover:bg-gray-800 transition-colors disabled:opacity-40"
          @click="doFinish"
          :disabled="finishing"
        >完成</button>
      </div>
    </div>

    <!-- Footer -->
    <div class="px-8 py-4 border-t border-gray-100 flex items-center justify-between">
      <p class="text-xs text-gray-300">Windows 人脸识别登录</p>
      <button class="text-xs text-gray-500 hover:text-gray-800" @click="openLicenses">许可说明</button>
    </div>
    <dialog ref="licenseDialog" aria-labelledby="license-title"
      class="m-auto w-[calc(100%-2rem)] max-w-3xl rounded-lg p-0 shadow-xl backdrop:bg-black/30">
      <div class="flex max-h-[85vh] flex-col">
        <div class="flex items-center justify-between border-b border-gray-100 px-5 py-4">
          <h2 id="license-title" class="text-sm font-medium text-gray-900">许可说明</h2>
          <button class="text-sm text-gray-500 hover:text-gray-800" @click="licenseDialog?.close()">关闭</button>
        </div>
        <div class="flex gap-4 border-b border-gray-100 px-5 pt-3">
          <button v-for="doc in licenseDocuments" :key="doc.path"
            :class="['pb-3 text-xs', licenseDocument === doc.path ? 'border-b-2 border-gray-900 text-gray-900' : 'text-gray-500']"
            :aria-pressed="licenseDocument === doc.path" @click="showLicenseDocument(doc.path)">{{ doc.label }}</button>
        </div>
        <pre class="min-h-0 overflow-auto whitespace-pre-wrap break-words px-5 py-4 text-xs leading-relaxed text-gray-700 select-text"
          aria-live="polite">{{ licenseText }}</pre>
      </div>
    </dialog>

    <dialog ref="consentDialog" aria-labelledby="consent-title" @cancel.prevent
      class="m-auto w-[calc(100%-2rem)] max-w-2xl overflow-hidden rounded-xl p-0 shadow-2xl backdrop:bg-slate-900/50">
      <div class="flex max-h-[92vh] flex-col bg-white">
        <div ref="consentScroller" tabindex="-1" autofocus
          class="min-h-0 flex-1 overflow-auto px-8 py-6 focus:outline-none" @scroll="onConsentScroll">
          <div v-if="consentLoading" class="flex items-center gap-2 py-8 text-xs text-gray-500">
            <span class="h-3 w-3 animate-spin rounded-full border-2 border-gray-200 border-t-gray-600"></span>
            正在读取隐私说明…
          </div>
          <div v-else-if="consentError" class="rounded-lg border border-red-100 bg-red-50 px-4 py-3 text-xs leading-relaxed text-red-700">
            {{ consentError }}<br>隐私说明无法显示，安装已禁用。请重新打开安装程序。
          </div>
          <article v-else class="mx-auto max-w-2xl">
            <template v-for="(block, index) in consentBlocks" :key="index">
              <h2 v-if="block.kind === 'title'" id="consent-title" class="mb-2 text-lg font-semibold tracking-tight text-gray-900">{{ block.text }}</h2>
              <p v-else-if="block.kind === 'meta'" class="mb-5 text-[11px] leading-6 text-gray-400">{{ block.text }}</p>
              <div v-else-if="block.kind === 'callout'" class="mb-5 rounded-lg border border-amber-100 bg-amber-50/80 px-4 py-3 text-[12px] leading-6 text-amber-900">
                {{ block.text }}
              </div>
              <h3 v-else-if="block.kind === 'heading'" class="mb-1.5 mt-5 text-[13px] font-semibold text-gray-900">{{ block.text }}</h3>
              <div v-else-if="block.kind === 'items'" class="mb-3 space-y-1.5">
                <p v-for="(item, itemIndex) in block.items" :key="itemIndex"
                  class="[overflow-wrap:anywhere] text-justify [text-justify:inter-ideograph] text-[13px] leading-7 text-gray-600">{{ item }}</p>
              </div>
              <p v-else class="mb-3 [overflow-wrap:anywhere] text-justify [text-justify:inter-ideograph] text-[13px] leading-7 text-gray-600">{{ block.text }}</p>
            </template>
            <div class="h-2"></div>
          </article>
        </div>

        <div class="border-t border-gray-100 bg-white px-8 py-4">
          <label v-if="consentScrolled || consentAccepted"
            class="flex cursor-pointer items-start gap-3 rounded-lg border border-gray-200 bg-gray-50/60 px-3.5 py-2.5 transition-colors">
            <input type="checkbox" v-model="consentChecked"
              :disabled="consentAccepted || !!consentError"
              class="mt-0.5 h-4 w-4 flex-shrink-0 accent-gray-900" />
            <span class="text-xs leading-relaxed text-gray-700">
              我已完整阅读并同意
              <span class="font-medium text-gray-900">《LockFace 人脸识别隐私说明与知情同意书》</span>
            </span>
          </label>
          <div class="mt-2.5 flex items-center justify-between gap-4">
            <span v-if="!consentScrolled && !consentAccepted && !consentError"
              class="text-[11px] text-gray-400">请滚动阅读至底部后勾选</span>
            <span v-else-if="consentAccepted" class="text-[11px] font-medium text-emerald-600">✓ 已阅读并同意</span>
            <button v-if="!consentAccepted"
              class="rounded-sm text-xs text-gray-500 underline decoration-gray-300 underline-offset-4 hover:text-gray-800 focus:outline-none focus-visible:ring-2 focus-visible:ring-gray-300 focus-visible:ring-offset-2"
              @click="declineConsent">不同意并退出</button>
            <button v-if="consentScrolled || consentAccepted"
              class="rounded-md bg-gray-900 px-6 py-2 text-xs font-medium text-white transition-colors hover:bg-gray-800 focus:outline-none focus-visible:ring-2 focus-visible:ring-gray-400 focus-visible:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-40"
              :disabled="!consentAccepted && !consentChecked" @click="acceptConsent">
              {{ consentAccepted ? '关闭' : '同意并继续' }}
            </button>
          </div>
        </div>
      </div>
    </dialog>
  </div>

</template>
