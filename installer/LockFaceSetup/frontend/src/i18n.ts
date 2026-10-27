import { ref } from 'vue'
import zh from './locales/zh-CN.json'
import en from './locales/en.json'
import { GetLanguageSettings, SetLanguageMode } from '../wailsjs/go/main/App'

export const locale = ref('zh-CN')
export const languageMode = ref('system')
const catalogs: Record<string, Record<string, string>> = { 'zh-CN': zh, en }

export function lfT(key: string, parameters?: Record<string, string | number>): string {
  const text = catalogs[locale.value]?.[key] ?? catalogs['zh-CN'][key] ?? key
  return text.replace(/\{(\w+)\}/g, (match, name: string) =>
    parameters && Object.hasOwn(parameters, name) ? String(parameters[name]) : match)
}

export async function initLanguage() {
  const settings = await GetLanguageSettings()
  locale.value = settings.locale
  languageMode.value = settings.mode
  document.documentElement.lang = locale.value
}

export async function setLanguageMode(mode: string) {
  if (!await SetLanguageMode(mode)) throw new Error(lfT('language_save_failed'))
  await initLanguage()
}
